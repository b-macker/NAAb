#!/usr/bin/env python3
"""Local LLM API stub for agent tests (python3 stdlib only).

Serves Gemini-shaped generateContent responses so tests can exercise the
post-receive half of agent.send()/agent.propose() without live API keys.
Point an agent at it via the per-agent govern.json field:

    "api_base": "http://127.0.0.1:<port>"

Usage:
    python3 agent_stub.py <port> <fixture.json> [state_dir]

Fixture format (responses consumed in order; last repeats when exhausted):
    {
      "responses": [
        {"content": "hello", "input_tokens": 10, "output_tokens": 20},
        {"tool_calls": [{"name": "get_data", "args": {"x": 1}}]},
        {"status": 500, "error": "internal"},
        {"content": ""}
      ]
    }

Optional per-agent routing (each route keeps its OWN counter):
    {
      "routes": {
        "AGENT-TOKEN-WORKER": {"responses": [ ... ]},
        "AGENT-TOKEN-JUDGE":  {"responses": [ ... ]}
      },
      "responses": [ ... ]            # fallback, unchanged semantics
    }

A route key is matched as a plain SUBSTRING of the raw request body, so the
harness gives each agent a unique token in its system_prompt. Without this,
one global counter interleaves every agent's queue and a scenario cannot give
one agent a scripted failure sequence while another stays clean.

Two rules that a fixture author has to be able to rely on:
  * First matching key in fixture order wins (json.load preserves object
    order). A body carrying two tokens is therefore resolved deterministically
    rather than by dict iteration luck -- but it is still a fixture bug, and
    it is easy to cause by accident, since conversation history replays
    earlier turns and a token pasted into one agent's prompt can end up in
    another agent's body.
  * Because that failure mode is silent (a plausible response from the wrong
    queue), every request's routing decision is logged to <state_dir>/routes.log
    as "<request-number> <route-key-or-->". Assert on it.

The global response index advances only for UNROUTED requests, so a fixture
with no "routes" key behaves exactly as before.

Prints "READY <port>" on stdout once listening. Writes request bodies to
<state_dir>/req_N.json when state_dir is given (for test assertions).

Also writes <state_dir>/keys.log, one line per request:

    <request-number> <api-key-value-or--> <start_ms> <end_ms>

The API key travels in a HEADER (x-goog-api-key / x-api-key), never the
body, so req_N.json cannot answer "which key did this call use". Start/end
are ms since stub start: OVERLAPPING windows are the only evidence that two
requests were genuinely in flight together rather than merely adjacent in
the log. Give a response "hold_ms" to keep its slot open long enough for
that overlap to be observable -- a stub that answers instantly makes every
arm look sequential no matter what the engine did.

A request's keys.log line is written BEFORE its response body is sent, so a
sequential client's lines are in request order and none is lost when the stub
is stopped after the last reply. "log_hold_ms" on a response waits that long
just before the line is written -- the probe for that ordering
(test_stub_routing.sh SR-08).

Tool-call pairing is validated as Gemini validates it: a turn of
functionResponse parts must follow a model turn holding the same number of
functionCall parts, with the same names. A request that breaks it is answered
400 with Gemini's own message and logged to <state_dir>/pairing.log, so an
engine that answers a call it never showed (A44(b)) fails here as it would
against the provider, instead of passing a stub that accepts anything.
"validate_tool_pairing": false in the fixture turns the check off.

Two keys let a fixture send what no well-behaved provider would (the
model-channel fuzzer, tests/fuzzing/model_channel/):
    {"raw_payload": {...}}     this JSON object is the whole response body,
                               in place of the generateContent shape
    {"raw_body": "..."}        these exact characters are the body, encoded
                               as Latin-1 so any byte 0-255 can be sent
                               (malformed JSON, truncated bodies, invalid UTF-8)
Both take "status" (default 200) and "hold_ms" as usual.

The server is threaded, so concurrent dispatch is served concurrently.
StubState is mutated only under its lock, so response-queue semantics are
unchanged from the single-threaded version.
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class StubState:
    def __init__(self, fixture_path, state_dir):
        with open(fixture_path, encoding="utf-8", errors="replace") as f:
            fixture = json.load(f)
        self.responses = fixture.get("responses", [])
        # Insertion-ordered: first matching key wins, and the fixture author
        # controls which that is.
        self.routes = fixture.get("routes", {}) or {}
        self.route_count = dict((k, 0) for k in self.routes)
        self.state_dir = state_dir
        self.validate_pairing = fixture.get("validate_tool_pairing", True)
        self.count = 0        # every request, for req_N.json numbering
        self.global_idx = 0   # advances only on UNROUTED requests
        self.lock = threading.Lock()
        # Appends to the shared logs (keys.log, routes.log, pairing.log) are
        # serialised. Two handler threads append at the same moment whenever two
        # requests are held together, and on Windows an append is not atomic
        # between file handles (the C runtime seeks to the end, then writes):
        # both lines land at one offset and one is lost. Observed on build-windows
        # (#354): test_tool_thread_vm.sh's two concurrent first calls left one
        # keys.log line in 3 of 3 runs. POSIX O_APPEND is atomic, so Linux never
        # showed it.
        self.log_lock = threading.Lock()

    def append_log(self, name, line):
        """Append one line to a shared log in state_dir, one writer at a time."""
        if not self.state_dir:
            return
        try:
            with self.log_lock:
                with open("%s/%s" % (self.state_dir, name), "a", encoding="utf-8", errors="replace") as f:
                    f.write(line)
        except OSError:
            pass

    def log_key(self, n, api_key, t0, t1):
        """Record which API key served request n, and when.

        The Gemini key travels in the x-goog-api-key HEADER, so a body-only
        log cannot see it -- and "which key did each concurrent slot pick"
        is unanswerable without this. t0/t1 are ms since stub start, so
        overlapping [t0,t1] windows are what proves requests were genuinely
        in flight together rather than merely adjacent in the log.
        """
        self.append_log("keys.log", "%d %s %d %d\n" % (n, api_key or "-", t0, t1))

    def _pick(self, responses, idx):
        if not responses:
            return {"content": "stub default response"}
        return responses[min(idx, len(responses) - 1)]

    def next_response(self, body, api_key=None):
        with self.lock:
            self.count += 1
            n = self.count
            key = None
            for k in self.routes:
                if k in body:
                    key = k
                    break
            if key is None:
                idx = self.global_idx
                self.global_idx += 1
                responses = self.responses
            else:
                idx = self.route_count[key]
                self.route_count[key] = idx + 1
                responses = self.routes[key].get("responses", [])
        if self.state_dir:
            try:
                with open("%s/req_%d.json" % (self.state_dir, n), "w", encoding="utf-8", errors="replace") as f:
                    f.write(body)
            except OSError:
                pass
            self.append_log("routes.log", "%d %s\n" % (n, key if key is not None else "-"))
        return self._pick(responses, idx), n


STATE = None
START_MONO = time.monotonic()

PAIRING_ERROR = ("Please ensure that the number of function response parts is "
                 "equal to the number of function call parts of the function call turn.")


def pairing_violation(request_body):
    """None when every functionResponse turn answers the model turn before it,
    else a one-line description of the first that does not."""
    try:
        contents = json.loads(request_body).get("contents") or []
    except (ValueError, AttributeError):
        return None   # not a generateContent body: nothing to check
    for i, turn in enumerate(contents):
        parts = turn.get("parts") or []
        responses = [p["functionResponse"].get("name", "") for p in parts
                     if isinstance(p, dict) and "functionResponse" in p]
        if not responses:
            continue
        prev = contents[i - 1] if i > 0 else {}
        calls = [p["functionCall"].get("name", "") for p in (prev.get("parts") or [])
                 if isinstance(p, dict) and "functionCall" in p]
        if prev.get("role") != "model" or sorted(calls) != sorted(responses):
            return "turn %d: responses %s answer calls %s" % (i, sorted(responses), sorted(calls))
    return None


def gemini_body(spec, request_body=""):
    parts = []
    content = spec.get("content")
    if content:
        parts.append({"text": content})
    for tc in spec.get("tool_calls", []):
        parts.append({"functionCall": {"name": tc.get("name", ""),
                                       "args": tc.get("args", {})}})
    if not parts:
        parts = [{"text": ""}]
    # thinking_tokens: null (or the JSON literal null) omits thoughtsTokenCount
    # from usageMetadata entirely, reproducing a provider that does not report
    # thinking at all. Absent and zero are different facts and the fixture has
    # to be able to express both.
    # promptTokenCount defaults to a size derived from the ACTUAL request body,
    # because a real provider's prompt count grows with the conversation and a
    # constant makes every input-token-keyed signal unobservable keylessly.
    # context_growth (S12) compares current input tokens against an early
    # baseline; with a fixed 10 it can never fire, so a keyless run cannot tell
    # a working context_window from a missing one -- the mechanism-never-ran
    # vacuity, sitting in the harness that exists to prevent it. An explicit
    # fixture "input_tokens" still wins, so no existing fixture changes.
    usage = {
        "promptTokenCount": spec.get("input_tokens",
                                     max(10, len(request_body) // 4)),
        "candidatesTokenCount": spec.get("output_tokens",
                                         max(1, len(content or "") // 4)),
    }
    if spec.get("thinking_tokens", 0) is not None:
        usage["thoughtsTokenCount"] = spec.get("thinking_tokens", 0)
    return {
        "candidates": [{
            "content": {"parts": parts, "role": "model"},
            "finishReason": spec.get("finish_reason", "STOP"),
        }],
        "usageMetadata": usage,
    }


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", errors="replace")
        api_key = (self.headers.get("x-goog-api-key")
                   or self.headers.get("x-api-key"))
        t0 = int((time.monotonic() - START_MONO) * 1000)
        spec, req_n = STATE.next_response(body, api_key)

        status = spec.get("status", 200)
        bad_pairing = STATE.validate_pairing and pairing_violation(body)
        if bad_pairing:
            STATE.append_log("pairing.log", "%d %s\n" % (req_n, bad_pairing))
            status = 400
            payload = {"error": {"code": 400, "message": PAIRING_ERROR,
                                 "status": "INVALID_ARGUMENT"}}
        elif "raw_body" in spec:
            payload = None
        elif "raw_payload" in spec:
            payload = spec["raw_payload"]
        elif status != 200:
            payload = {"error": {"message": spec.get("error", "stub error"),
                                 "status": str(status)}}
        else:
            payload = gemini_body(spec, body)

        if payload is None and not bad_pairing:
            data = str(spec["raw_body"]).encode("latin-1", errors="replace")
        else:
            data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        # Hold the slot open so genuinely-concurrent requests overlap in
        # wall time. Without this a fast stub answers slot 1 before slot 2
        # is even dispatched, and every arm looks sequential regardless of
        # what the engine did.
        hold = spec.get("hold_ms", 0)
        if hold:
            time.sleep(hold / 1000.0)
        # Log BEFORE the body goes out: a client cannot act on this response
        # until the body arrives, so request N's line is written before
        # request N+1 can exist. Logged after the write, a sequential client's
        # next request could log first -- or the stub be stopped before this
        # line was written at all, once the program had its last reply.
        # Observed under one CPU (tests/tsan/schedule_perturb.sh): CK-11 read
        # request 2's key on line 1, in 1 of 3 rounds; a delay planted before
        # the old log call lost request 1's line in 3 of 3 runs. t1 still
        # follows the hold, which is the window CK-03 and the overlap probes
        # compare.
        log_hold = spec.get("log_hold_ms", 0)
        if log_hold:
            time.sleep(log_hold / 1000.0)
        t1 = int((time.monotonic() - START_MONO) * 1000)
        STATE.log_key(req_n, api_key, t0, t1)
        self.wfile.write(data)

    def log_message(self, fmt, *args):  # silence per-request stderr noise
        pass


def main():
    if len(sys.argv) < 3:
        print("usage: agent_stub.py <port> <fixture.json> [state_dir]",
              file=sys.stderr)
        return 2
    port = int(sys.argv[1])
    state_dir = sys.argv[3] if len(sys.argv) > 3 else None
    global STATE
    STATE = StubState(sys.argv[2], state_dir)
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("READY %d" % server.server_address[1], flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
