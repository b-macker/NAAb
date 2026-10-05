#!/usr/bin/env python3
"""LLM bridge: a loopback Gemini generateContent endpoint answered by a human
or agent "director" through a file queue (python3 stdlib only).

WHY THIS EXISTS

Every agent test in this repository talks to tests/helpers/agent_stub.py,
which replays canned, well-formed responses. Governance has therefore only
ever been exercised against cases someone wrote down. This bridge lets a
director (a person, or a Claude session driving subagent "personas") answer
each request with live, varied, imperfect model output -- with no API key --
so the engine's reactions to real behaviour, including false positives on
GOOD output, can be observed. It is an exploratory instrument, NOT for CI:
it is slow and non-deterministic by design. What it finds is pinned for CI
by exporting the captured exchange as an agent_stub.py fixture
(`export-fixture`), which replays deterministically.

PROTOCOL

NAAb sends Gemini-format requests to a per-agent `api_base`; plain http is
accepted by the engine only for loopback hosts, which is where this binds.
For every POST to /v1beta/models/<model>:generateContent the bridge

  1. writes   <queue>/requests/<n>.json      (structured: route, system
                                             prompt, messages, tools,
                                             generation config)
              <queue>/requests/<n>.raw.json  (the body exactly as received)
  2. blocks until <queue>/responses/<n>.json exists (written by `respond`
     or `autoreply`), then
  3. returns a Gemini response built from it, shaped exactly as
     agent_stub.py's gemini_body() shapes its own.

Request numbers are global across agents; `route_seq` numbers the requests
of one route (agent role), which is what an intervention ledger refers to.

TOKEN COUNTS ARE ESTIMATES -- READ THIS BEFORE USING ANY TOKEN-KEYED SIGNAL

The director produces text, not tokens. usageMetadata is therefore an
ESTIMATE: ceil(max(words * 1.3, chars / 4)) -- see est_tokens() for
why not words*1.3 alone -- for the prompt (system
instruction + every text part + function call/response JSON + tool
declarations) and for the candidate (content + function call JSON).
`thoughtsTokenCount` is OMITTED, i.e. thinking is UNREPORTED (absent, not
zero -- see ProviderResponse.thinking_reported). Every response logged in
bridge_log.jsonl carries "tokens_estimated": true. Findings that rest on a
token-keyed CDD signal -- S8 response_quality, S9 thinking_collapse, S12
context_growth, S23 response_degenerate -- rest on this estimate, and must
say so.

MAX_TOKENS is emulated: when the estimated candidate exceeds the request's
generationConfig.maxOutputTokens, the text is cut at the word that reaches
the limit and finishReason is MAX_TOKENS, as a real provider would. Logged as
"truncated_by_bridge": true. Disable with `serve --no-truncate`.

PROVENANCE

`respond` records who produced each response (persona, model, style profile,
intervention id) in responses/<n>.json; the server copies it into the
bridge_log.jsonl "response" line, so every response in the log names its
source. The raw reply text is kept verbatim in responses/<n>.reply.txt.

USAGE (see README.md)

  bridge.py serve      --queue Q [--port 0] [--response-timeout 7200]
  bridge.py next       --queue Q [--wait 600]     # render pending requests
  bridge.py render     --queue Q N
  bridge.py respond    --queue Q N --persona P --model M [--style S]
                       [--intervention ID] [--reply-file F]   (else stdin)
  bridge.py wait       --queue Q [--seconds 25]   # idle wait, prints status
  bridge.py autoreply  --queue Q --fixture stub_fixture.json  # control run
  bridge.py export-fixture --queue Q > fixture.json
"""
import argparse
import datetime
import hashlib
import json
import math
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKENS_PER_WORD = 1.3
DEFAULT_ROUTE_RE = r"^\s*([A-Z][A-Z0-9]*-[A-Z][A-Z0-9_]*)\b"
EXIT_MARKER = "naab_exit"          # written by the run script when NAAb exits
CALL_RE = re.compile(r"^\s*CALL\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\{.*\})?\s*$")


# ---------------------------------------------------------------- helpers ----

def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="milliseconds")


def est_tokens(text):
    """ESTIMATED token count: ceil(max(words * 1.3, chars / 4)). Not a tokenizer.

    words*1.3 alone undercounts compact JSON, whose punctuation a real
    tokenizer counts: the control run (stub fixture through the bridge) made
    the critic's `{"verdict": "APPROVED", "issues": []}` 6 tokens -- below
    S23 response_degenerate's 8-token floor, so S23 fired on a response the
    stub run (and a real tokenizer, ~10-12) does not flag. chars/4 is the
    engine's own fallback when a provider omits candidatesTokenCount
    (agent_provider.cpp normalizeResponse). Taking the max keeps prose near
    words*1.3 and stops JSON being undercounted.
    """
    words = len(text.split())
    return int(math.ceil(max(words * TOKENS_PER_WORD, len(text) / 4.0)))


def truncate_to_tokens(text, max_tokens):
    """Longest word-boundary prefix whose est_tokens() fits max_tokens."""
    if max_tokens <= 0:
        return ""
    if est_tokens(text) <= max_tokens:
        return text
    best = ""
    for m in re.finditer(r"\S+", text):
        cand = text[:m.end()]
        if est_tokens(cand) > max_tokens:
            break
        best = cand
    return best


def atomic_write(path, data):
    tmp = "%s.tmp.%d.%d" % (path, os.getpid(), threading.get_ident())
    with open(tmp, "w", encoding="utf-8", errors="strict") as f:
        f.write(data)
    os.replace(tmp, path)


def read_json(path):
    with open(path, encoding="utf-8", errors="strict") as f:
        return json.load(f)


def qpaths(queue):
    return (os.path.join(queue, "requests"), os.path.join(queue, "responses"),
            os.path.join(queue, "bridge_log.jsonl"))


def log_line(queue, entry, lock=None):
    _, _, log = qpaths(queue)
    line = json.dumps(entry, sort_keys=True) + "\n"
    if lock:
        with lock:
            with open(log, "a", encoding="utf-8") as f:
                f.write(line)
    else:
        with open(log, "a", encoding="utf-8") as f:
            f.write(line)


def part_text(part):
    """Every part's contribution to the prompt, as text (for token estimates)."""
    if "text" in part and isinstance(part["text"], str):
        return part["text"]
    if "functionCall" in part:
        return json.dumps(part["functionCall"])
    if "functionResponse" in part:
        return json.dumps(part["functionResponse"])
    return ""


def structure_request(body, route_re):
    sys_text = ""
    si = body.get("systemInstruction") or {}
    for p in si.get("parts", []) or []:
        if isinstance(p.get("text"), str):
            sys_text += p["text"]
    tools = []
    for t in body.get("tools", []) or []:
        tools.extend(t.get("functionDeclarations", []) or [])
    messages = body.get("contents", []) or []
    m = re.search(route_re, sys_text)
    route = m.group(1) if m else "-"
    prompt_text = sys_text + "\n" + (json.dumps(tools) if tools else "") + "\n" + "\n".join(
        part_text(p) for msg in messages for p in msg.get("parts", []) or [])
    return {
        "route": route,
        "system_prompt": sys_text,
        "tools": tools,
        "messages": messages,
        "generation_config": body.get("generationConfig", {}) or {},
        "prompt_tokens_estimated": est_tokens(prompt_text),
    }


# ----------------------------------------------------------------- server ----

class BridgeState:
    def __init__(self, queue, route_re, response_timeout, truncate):
        self.queue = queue
        self.route_re = route_re
        self.response_timeout = response_timeout
        self.truncate = truncate
        self.lock = threading.Lock()
        self.log_lock = threading.Lock()
        self.count = 0
        self.route_count = {}


STATE = None


def gemini_payload(spec, req, state):
    """Build the Gemini response body. Shape matches agent_stub.gemini_body()."""
    content = spec.get("content") or ""
    tool_calls = spec.get("tool_calls", []) or []
    finish = spec.get("finish_reason", "STOP")
    truncated = False
    max_out = req["generation_config"].get("maxOutputTokens")
    call_text = " ".join(json.dumps(tc) for tc in tool_calls)
    call_est = est_tokens(call_text) if tool_calls else 0
    out_est = est_tokens(content) + call_est
    if (state.truncate and isinstance(max_out, int) and max_out > 0
            and out_est > max_out and content):
        content = truncate_to_tokens(content, max_out - call_est)
        finish = "MAX_TOKENS"
        truncated = True
        out_est = max_out
    parts = []
    if content:
        parts.append({"text": content})
    for tc in tool_calls:
        parts.append({"functionCall": {"name": tc.get("name", ""),
                                       "args": tc.get("args", {}) or {}}})
    if not parts:
        parts = [{"text": ""}]
    # thoughtsTokenCount deliberately omitted: thinking is UNREPORTED.
    usage = {"promptTokenCount": req["prompt_tokens_estimated"],
             "candidatesTokenCount": out_est,
             "totalTokenCount": req["prompt_tokens_estimated"] + out_est}
    payload = {"candidates": [{"content": {"parts": parts, "role": "model"},
                               "finishReason": finish}],
               "usageMetadata": usage}
    return payload, {"finish_reason": finish, "truncated_by_bridge": truncated,
                     "output_tokens_estimated": out_est,
                     "content_returned": content}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, status, payload):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        st = STATE
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8", errors="replace")
        t0 = time.time()
        m = re.match(r"^/v1beta/models/([^:/]+):generateContent", self.path)
        if not m:
            self._send(404, {"error": {"message": "unknown path " + self.path,
                                       "status": "NOT_FOUND"}})
            return
        try:
            body = json.loads(raw)
        except ValueError:
            self._send(400, {"error": {"message": "body is not JSON",
                                       "status": "INVALID_ARGUMENT"}})
            return
        req = structure_request(body, st.route_re)
        with st.lock:
            st.count += 1
            n = st.count
            st.route_count[req["route"]] = st.route_count.get(req["route"], 0) + 1
            route_seq = st.route_count[req["route"]]
        req.update({"n": n, "route_seq": route_seq, "received_at": now_iso(),
                    "url_model": m.group(1)})
        reqdir, respdir, _ = qpaths(st.queue)
        atomic_write(os.path.join(reqdir, "%d.raw.json" % n), raw)
        atomic_write(os.path.join(reqdir, "%d.json" % n),
                     json.dumps(req, indent=1))
        log_line(st.queue, {"event": "request", "n": n, "t": req["received_at"],
                            "route": req["route"], "route_seq": route_seq,
                            "url_model": m.group(1),
                            "n_messages": len(req["messages"]),
                            "tools": [t.get("name") for t in req["tools"]],
                            "generation_config": req["generation_config"],
                            "prompt_tokens_estimated": req["prompt_tokens_estimated"],
                            "tokens_estimated": True}, st.log_lock)

        resp_path = os.path.join(respdir, "%d.json" % n)
        deadline = t0 + st.response_timeout
        while not os.path.exists(resp_path):
            if time.time() > deadline:
                log_line(st.queue, {"event": "TIMEOUT", "n": n, "t": now_iso(),
                                    "route": req["route"],
                                    "waited_s": st.response_timeout}, st.log_lock)
                self._send(504, {"error": {"message": "bridge: no response "
                                           "within %ds" % st.response_timeout,
                                           "status": "DEADLINE_EXCEEDED"}})
                return
            time.sleep(0.2)
        try:
            spec = read_json(resp_path)
        except (OSError, ValueError) as e:
            log_line(st.queue, {"event": "BAD_RESPONSE_FILE", "n": n,
                                "t": now_iso(), "error": str(e)}, st.log_lock)
            self._send(500, {"error": {"message": "bridge: unreadable response "
                                       "file", "status": "INTERNAL"}})
            return

        status = spec.get("status", 200)
        entry = {"event": "response", "n": n, "t": now_iso(),
                 "route": req["route"], "route_seq": route_seq,
                 "latency_s": round(time.time() - t0, 3), "http_status": status,
                 "provenance": spec.get("provenance", {}),
                 "tokens_estimated": True,
                 "thoughts_token_count": "omitted (unreported)",
                 "prompt_tokens_estimated": req["prompt_tokens_estimated"]}
        if status != 200:
            payload = {"error": {"message": spec.get("error", "bridge error"),
                                 "status": str(status)}}
            entry["error"] = spec.get("error", "")
        else:
            payload, info = gemini_payload(spec, req, st)
            content = info.pop("content_returned")
            entry.update(info)
            entry["tool_calls"] = spec.get("tool_calls", []) or []
            entry["content_length"] = len(content)
            entry["content_sha256"] = hashlib.sha256(
                content.encode("utf-8")).hexdigest()
        self._send(status, payload)
        log_line(st.queue, entry, st.log_lock)

    def log_message(self, fmt, *args):
        pass


def cmd_serve(a):
    global STATE
    reqdir, respdir, _ = qpaths(a.queue)
    os.makedirs(reqdir, exist_ok=True)
    os.makedirs(respdir, exist_ok=True)
    STATE = BridgeState(a.queue, a.route_regex, a.response_timeout,
                        not a.no_truncate)
    server = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    server.daemon_threads = True
    port = server.server_address[1]
    atomic_write(os.path.join(a.queue, "port"), "%d\n" % port)
    log_line(a.queue, {"event": "serve", "t": now_iso(), "port": port,
                       "tokens_per_word": TOKENS_PER_WORD,
                       "truncate": not a.no_truncate,
                       "response_timeout_s": a.response_timeout})
    print("READY %d" % port, flush=True)
    server.serve_forever()
    return 0


# ---------------------------------------------------------- director side ----

def describe_params(params):
    """Render a tool's parameter schema compactly, whatever its shape."""
    if not params:
        return ""
    props = params.get("properties") if isinstance(params, dict) else None
    if not isinstance(props, dict):
        props = params if isinstance(params, dict) else {}
    out = []
    for name, spec in props.items():
        if isinstance(spec, dict):
            d = "%s: %s" % (name, spec.get("type", "any"))
            if spec.get("description"):
                d += " -- " + spec["description"]
        else:
            d = name
        out.append(d)
    return "; ".join(out)


def render_request(req):
    """The persona-facing view: exactly what the request contains."""
    lines = ["=== API REQUEST ===", "", "[System instructions]",
             req["system_prompt"] or "(none)", ""]
    if req["tools"]:
        lines.append("[Tools you may call]")
        for t in req["tools"]:
            p = describe_params(t.get("parameters"))
            lines.append("- %s(%s): %s" % (t.get("name"), p,
                                           t.get("description", "")))
        lines.append("")
    gc = req["generation_config"]
    settings = []
    if "temperature" in gc:
        settings.append("temperature=%s" % gc["temperature"])
    if "maxOutputTokens" in gc:
        settings.append("max output tokens=%s" % gc["maxOutputTokens"])
    if settings:
        lines += ["[Generation settings] " + ", ".join(settings), ""]
    lines.append("[Conversation]")
    for msg in req["messages"]:
        role = msg.get("role")
        for p in msg.get("parts", []) or []:
            if "text" in p:
                head = "--- user ---" if role == "user" else "--- model (you) ---"
                lines += [head, p["text"]]
            elif "functionCall" in p:
                fc = p["functionCall"]
                lines += ["--- model (you) called a tool ---",
                          "CALL %s %s" % (fc.get("name"),
                                          json.dumps(fc.get("args", {})))]
            elif "functionResponse" in p:
                fr = p["functionResponse"]
                res = fr.get("response", {})
                if isinstance(res, dict) and set(res) == {"result"}:
                    res = res["result"]
                if not isinstance(res, str):
                    res = json.dumps(res)
                lines += ["--- tool result (%s) ---" % fr.get("name"), res]
    lines += ["", "=== END REQUEST ===",
              "Write the model's next turn."]
    return "\n".join(lines)


def pending(queue):
    reqdir, respdir, _ = qpaths(queue)
    out = []
    if not os.path.isdir(reqdir):      # bridge not started yet
        return out
    for f in os.listdir(reqdir):
        mm = re.match(r"^(\d+)\.json$", f)
        if mm and not os.path.exists(os.path.join(respdir, f)):
            out.append(int(mm.group(1)))
    return sorted(out)


def exit_marker(queue):
    p = os.path.join(queue, EXIT_MARKER)
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            return f.read().strip() or "?"
    return None


def cmd_render(a):
    reqdir, _, _ = qpaths(a.queue)
    print(render_request(read_json(os.path.join(reqdir, "%d.json" % a.n))))
    return 0


def print_pending(queue, ns):
    reqdir, _, _ = qpaths(queue)
    for n in ns:
        req = read_json(os.path.join(reqdir, "%d.json" % n))
        print("##### PENDING n=%d route=%s route_seq=%d messages=%d "
              "prompt_tokens_est=%d" % (n, req["route"], req["route_seq"],
                                        len(req["messages"]),
                                        req["prompt_tokens_estimated"]))
        print(render_request(req))
        print("##### END n=%d" % n)


def cmd_next(a):
    deadline = time.time() + a.wait
    while True:
        ns = pending(a.queue)
        if ns:
            # Let a burst (fan_out) land together before rendering.
            time.sleep(1.0)
            print_pending(a.queue, pending(a.queue))
            return 0
        rc = exit_marker(a.queue)
        if rc is not None:
            print("RUN_FINISHED rc=%s" % rc)
            return 3
        if time.time() > deadline:
            print("NO_PENDING after %ds" % a.wait)
            return 4
        time.sleep(0.5)


def cmd_wait(a):
    """Idle wait that ends early when a NEW request arrives or the run ends."""
    before = set(pending(a.queue))
    deadline = time.time() + a.seconds
    while time.time() < deadline:
        now = set(pending(a.queue))
        if now - before or exit_marker(a.queue) is not None:
            break
        time.sleep(0.5)
    print("pending=%s exit=%s" % (pending(a.queue), exit_marker(a.queue)))
    return 0


def parse_reply(text):
    """Persona reply -> (content, tool_calls). CALL lines become functionCalls.

    A CALL line whose JSON does not parse stays in the content as text -- a
    real model's malformed tool call is its own mistake, not ours to repair.
    """
    content_lines, calls = [], []
    for line in text.split("\n"):
        mm = CALL_RE.match(line)
        if mm:
            args_s = mm.group(2)
            try:
                args = json.loads(args_s) if args_s else {}
                if isinstance(args, dict):
                    calls.append({"name": mm.group(1), "args": args})
                    continue
            except ValueError:
                pass
        content_lines.append(line)
    return "\n".join(content_lines).strip(), calls


REQUEST_MARK = "=== API REQUEST ==="
NUDGE_MARK = "[handback-send-enforce]"


def _user_text(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(b.get("text", "") for b in content
                         if isinstance(b, dict) and b.get("type") == "text")
    return ""


def transcript_replies(path):
    """One reply per director request, in order, from a subagent transcript.

    A director request is a user message containing REQUEST_MARK. The reply
    to it is the persona's output BEFORE the next user message of any kind:
      * a SubagentHandback tool call in that segment -> its message
        (reply_source "handback"), else
      * the text blocks of that segment (reply_source "pre-nudge-text").
    Why the second form exists: a persona that ends its turn with plain text
    is NUDGED by the agent harness ("[handback-send-enforce] ... Call
    SubagentHandback ...") and its post-nudge output is a reaction to the
    harness, not to the request -- haiku personas under the CALL-line
    protocol answer every nudge with text-form fake handbacks. Their FIRST
    output is the genuine reply, so only pre-nudge output is ever served.
    A segment is complete once the persona hands back or a later user
    message (a nudge) exists; an incomplete trailing segment is not returned.
    Any tool use other than SubagentHandback is a fidelity breach (personas
    are told to work only from the request) and is reported per reply.
    """
    entries = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            m = e.get("message")
            if isinstance(m, dict) and m.get("role") in ("user", "assistant"):
                entries.append(m)
    replies = []
    i = 0
    while i < len(entries):
        m = entries[i]
        if m["role"] == "user" and REQUEST_MARK in _user_text(m.get("content")):
            texts, handback, others = [], None, []
            j = i + 1
            complete = False
            while j < len(entries):
                mj = entries[j]
                if mj["role"] == "user":
                    # Only a delivery NUDGE or the next director request ends
                    # a segment. The harness also injects user-role messages
                    # that are not the persona's doing -- a <system-reminder>
                    # BETWEEN the request and the reply, tool_results -- and
                    # those must not cut the segment (they did, in this
                    # function's first version, and every reply came back
                    # empty).
                    ut = _user_text(mj.get("content"))
                    if NUDGE_MARK in ut or REQUEST_MARK in ut:
                        complete = True
                        break
                    j += 1
                    continue
                for b in mj.get("content") or []:
                    if not isinstance(b, dict):
                        continue
                    if b.get("type") == "text":
                        texts.append(b.get("text", ""))
                    elif b.get("type") == "tool_use":
                        if b.get("name") == "SubagentHandback" and handback is None:
                            handback = (b.get("input") or {}).get("message", "")
                        elif b.get("name") != "SubagentHandback":
                            others.append(b.get("name"))
                j += 1
            if handback is not None:
                replies.append((handback, others, "handback"))
            elif complete:
                replies.append(("\n".join(texts), others, "pre-nudge-text"))
            else:
                break   # trailing segment still in progress
            i = j
        else:
            i += 1
    return replies


def take_handback(queue, path, timeout):
    """Next UNUSED reply of a persona transcript, waiting for it to appear.

    Consumption is tracked per transcript in <queue>/handbacks_used.json, so
    a stale reply can never be served for a new request."""
    state_p = os.path.join(queue, "handbacks_used.json")
    state = read_json(state_p) if os.path.exists(state_p) else {}
    used = state.get(path, 0)
    deadline = time.time() + timeout
    while True:
        if os.path.exists(path):
            replies = transcript_replies(path)
            if len(replies) > used:
                text, others, source = replies[used]
                state[path] = used + 1
                atomic_write(state_p, json.dumps(state, indent=1))
                return text, others, "%d:%s" % (used + 1, source)
        if time.time() > deadline:
            return None, None, used
        time.sleep(1.0)


def cmd_respond(a):
    reqdir, respdir, _ = qpaths(a.queue)
    req = read_json(os.path.join(reqdir, "%d.json" % a.n))
    if os.path.exists(os.path.join(respdir, "%d.json" % a.n)):
        print("response %d already written -- refusing to overwrite" % a.n,
              file=sys.stderr)
        return 2
    other_tools, handback_index = [], None
    if a.from_transcript:
        text, other_tools, handback_index = take_handback(
            a.queue, a.from_transcript, a.reply_timeout)
        if text is None:
            print("NO_HANDBACK: no new reply in %s within %ds" % (
                a.from_transcript, a.reply_timeout))
            return 5
    elif a.reply_file:
        with open(a.reply_file, encoding="utf-8") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    if text.endswith("\n"):
        text = text[:-1]
    atomic_write(os.path.join(respdir, "%d.reply.txt" % a.n), text)
    content, calls = parse_reply(text)
    spec = {"content": content, "tool_calls": calls,
            "provenance": {"persona": a.persona, "model": a.model,
                           "style": a.style or "none",
                           "intervention": a.intervention or "",
                           "director_note": a.note or "",
                           "reply_source": ("transcript:%s#%s" % (
                               os.path.basename(a.from_transcript),
                               handback_index) if a.from_transcript
                               else "director-supplied text"),
                           "persona_other_tool_uses": other_tools,
                           "route": req["route"], "route_seq": req["route_seq"],
                           "written_at": now_iso()}}
    atomic_write(os.path.join(respdir, "%d.json" % a.n),
                 json.dumps(spec, indent=1))
    print("RESPONDED n=%d route=%s content_chars=%d tool_calls=%s%s" % (
        a.n, req["route"], len(content), [c["name"] for c in calls],
        (" PERSONA_USED_TOOLS=%s" % other_tools) if other_tools else ""))
    print("----- reply as served (first 1500 chars) -----")
    print(text[:1500])
    print("-----")
    if a.then_next is not None:
        sys.stdout.flush()
        a.wait = a.then_next
        return cmd_next(a)
    return 0


def cmd_autoreply(a):
    """Answer requests from an agent_stub.py fixture, by route, in order.

    This is the CONTROL: the same harness through the bridge, answered with
    the stub's own canned responses, must produce the stub's green result.
    If it does not, the bridge or the run setup is broken, and nothing a
    persona run shows can be trusted.
    """
    fixture = read_json(a.fixture)
    routes = fixture.get("routes", {}) or {}
    counters = dict((k, 0) for k in routes)
    gidx = 0
    reqdir, respdir, _ = qpaths(a.queue)
    while exit_marker(a.queue) is None:
        for n in pending(a.queue):
            req = read_json(os.path.join(reqdir, "%d.json" % n))
            route = req["route"] if req["route"] in routes else None
            if route is None:
                lst = fixture.get("responses", [])
                spec = dict(lst[min(gidx, len(lst) - 1)]) if lst else {}
                gidx += 1
            else:
                lst = routes[route].get("responses", [])
                spec = dict(lst[min(counters[route], len(lst) - 1)])
                counters[route] += 1
            spec["provenance"] = {"persona": "autoreply", "model": "fixture",
                                  "style": "none", "route": req["route"],
                                  "route_seq": req["route_seq"],
                                  "written_at": now_iso()}
            atomic_write(os.path.join(respdir, "%d.json" % n),
                         json.dumps(spec, indent=1))
        time.sleep(0.2)
    print("autoreply: run finished rc=%s" % exit_marker(a.queue))
    return 0


def cmd_export_fixture(a):
    """Captured exchange -> agent_stub.py fixture (routes, in arrival order).

    Token counts are exported explicitly, so a replay reproduces the bridge's
    ESTIMATES rather than the stub's own body-length heuristic; thinking stays
    unreported (thinking_tokens: null). Replay fidelity is exact only while
    the engine sends the same request sequence -- a code change that alters
    the sequence shifts every later response, as with any stub fixture.
    """
    _, respdir, log = qpaths(a.queue)
    responses = {}
    with open(log, encoding="utf-8") as f:
        for line in f:
            e = json.loads(line)
            if e.get("event") == "response":
                responses[e["n"]] = e
    routes = {}
    for n in sorted(responses):
        e = responses[n]
        spec = read_json(os.path.join(respdir, "%d.json" % n))
        if e.get("http_status", 200) != 200:
            item = {"status": e["http_status"], "error": e.get("error", "")}
        else:
            item = {"content": spec.get("content", "")}
            if e.get("truncated_by_bridge"):
                # Replay what was actually returned, not what was written.
                calls = spec.get("tool_calls", []) or []
                call_est = est_tokens(" ".join(
                    json.dumps(tc) for tc in calls)) if calls else 0
                item["content"] = truncate_to_tokens(
                    item["content"], e["output_tokens_estimated"] - call_est)
                item["finish_reason"] = "MAX_TOKENS"
            if spec.get("tool_calls"):
                item["tool_calls"] = spec["tool_calls"]
            item["input_tokens"] = e["prompt_tokens_estimated"]
            item["output_tokens"] = e["output_tokens_estimated"]
            item["thinking_tokens"] = None
        item["_provenance"] = e.get("provenance", {})
        item["_bridge_n"] = n
        routes.setdefault(e["route"], {"responses": []})["responses"].append(item)
    out = {"_comment": "Exported by tools/llm_bridge/bridge.py export-fixture "
                       "from %s. Token counts are the bridge's ESTIMATES "
                       "(words*1.3); thinking unreported." % a.queue,
           "routes": routes,
           "responses": [{"content": "{\"error\": \"unrouted request\"}"}]}
    print(json.dumps(out, indent=1))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve")
    s.add_argument("--queue", required=True)
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--route-regex", default=DEFAULT_ROUTE_RE)
    s.add_argument("--response-timeout", type=int, default=7200)
    s.add_argument("--no-truncate", action="store_true")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("next")
    s.add_argument("--queue", required=True)
    s.add_argument("--wait", type=int, default=600)
    s.set_defaults(fn=cmd_next)

    s = sub.add_parser("render")
    s.add_argument("--queue", required=True)
    s.add_argument("n", type=int)
    s.set_defaults(fn=cmd_render)

    s = sub.add_parser("wait")
    s.add_argument("--queue", required=True)
    s.add_argument("--seconds", type=int, default=25)
    s.set_defaults(fn=cmd_wait)

    s = sub.add_parser("respond")
    s.add_argument("--queue", required=True)
    s.add_argument("n", type=int)
    s.add_argument("--persona", required=True)
    s.add_argument("--model", required=True)
    s.add_argument("--style", default="")
    s.add_argument("--intervention", default="")
    s.add_argument("--note", default="")
    s.add_argument("--reply-file")
    s.add_argument("--from-transcript",
                   help="subagent transcript JSONL; serve its next unused "
                        "SubagentHandback message verbatim (waits for it)")
    s.add_argument("--reply-timeout", type=int, default=900)
    s.add_argument("--then-next", type=int, default=None,
                   help="after responding, wait up to N s and render pending")
    s.set_defaults(fn=cmd_respond)

    s = sub.add_parser("autoreply")
    s.add_argument("--queue", required=True)
    s.add_argument("--fixture", required=True)
    s.set_defaults(fn=cmd_autoreply)

    s = sub.add_parser("export-fixture")
    s.add_argument("--queue", required=True)
    s.set_defaults(fn=cmd_export_fixture)

    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
