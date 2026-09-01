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
        with open(fixture_path) as f:
            fixture = json.load(f)
        self.responses = fixture.get("responses", [])
        # Insertion-ordered: first matching key wins, and the fixture author
        # controls which that is.
        self.routes = fixture.get("routes", {}) or {}
        self.route_count = dict((k, 0) for k in self.routes)
        self.state_dir = state_dir
        self.count = 0        # every request, for req_N.json numbering
        self.global_idx = 0   # advances only on UNROUTED requests
        self.lock = threading.Lock()

    def log_key(self, n, api_key, t0, t1):
        """Record which API key served request n, and when.

        The Gemini key travels in the x-goog-api-key HEADER, so a body-only
        log cannot see it -- and "which key did each concurrent slot pick"
        is unanswerable without this. t0/t1 are ms since stub start, so
        overlapping [t0,t1] windows are what proves requests were genuinely
        in flight together rather than merely adjacent in the log.
        """
        if not self.state_dir:
            return
        try:
            with open("%s/keys.log" % self.state_dir, "a") as f:
                f.write("%d %s %d %d\n" % (n, api_key or "-", t0, t1))
        except OSError:
            pass

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
                with open("%s/req_%d.json" % (self.state_dir, n), "w") as f:
                    f.write(body)
            except OSError:
                pass
            try:
                with open("%s/routes.log" % self.state_dir, "a") as f:
                    f.write("%d %s\n" % (n, key if key is not None else "-"))
            except OSError:
                pass
        return self._pick(responses, idx), n


STATE = None
START_MONO = time.monotonic()


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
        if status != 200:
            payload = {"error": {"message": spec.get("error", "stub error"),
                                 "status": str(status)}}
        else:
            payload = gemini_body(spec, body)

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
        self.wfile.write(data)
        t1 = int((time.monotonic() - START_MONO) * 1000)
        STATE.log_key(req_n, api_key, t0, t1)

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
