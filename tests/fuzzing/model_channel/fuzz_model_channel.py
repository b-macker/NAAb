#!/usr/bin/env python3
"""Model-channel fuzzer (Bundle 9): what a model sends back must never break the governor.

Each case is a seed. The seed picks a short sequence of model replies -- empty,
huge, odd encodings, malformed or truncated bodies, odd JSON shapes, tool calls
with wrong names, types and counts, HTTP errors, a secret-shaped reply -- and
tests/helpers/agent_stub.py serves them to a NAAb program that sends three
messages to an agent with a tool, each inside try/catch. The invariant, per case:

  ends      the run finishes inside its time limit (no hang)
  exit      0, 3 (a HARD block) or 4 (a config error); never a signal, never 1
  finished  an exit 0 run printed FUZZ-DONE (it was not cut short)
  evidence  the telemetry file exists and --verify-telemetry-chain accepts it,
            and the governance report exists and parses
  pairing   the stub logged no tool-call pairing violation (the engine never
            sent a request a provider would reject)

Verdicts: PASS, FAIL (with the class), UNMEASURABLE (the stub did not start).

--plant MODE replaces the interpreter with a stand-in that breaks one invariant
(crash, hang, exit1, noevidence, pairing) -- the controls that each check can
fail. A plant must FAIL every case; the suite asserts it.

Usage:
  fuzz_model_channel.py --naab build/naab-lang [--seeds 1-40] [--plant MODE]
                        [--keep DIR] [--timeout 60] [--show SEED]
Exit: 0 when every case PASSes (or, with --plant, when every case FAILs as
planted), 1 otherwise. Output is ASCII.
"""
import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
STUB = os.path.join(REPO, "tests", "helpers", "agent_stub.py")


def out(line):
    sys.stdout.buffer.write((line + "\n").encode("ascii", "replace"))
    sys.stdout.flush()


# ---- reply shapes ---------------------------------------------------------
# Each returns one stub response spec. Built from fragments where a value would
# otherwise be a literal secret or PII.

def s_empty(r):
    return {"content": ""}


def s_huge(r):
    n = r.choice([100_000, 1_000_000, 3_000_000])
    unit = r.choice(["x", "word ", "line\n", "{\"a\": [1, 2]} "])
    return {"content": (unit * (n // len(unit) + 1))[:n]}


def s_unicode(r):
    pieces = ["\u202e", "\u200b", "\u0000", "\ufeff", "e\u0301", "\U0001f600",
              "\u0645\u0631\u062d\u0628\u0627", "\u4e2d\u6587", "\x7f", "\x1b[31m",
              "\ud800", "\udfff", "\u2028", "\u0085"]
    return {"content": "".join(r.choice(pieces) for _ in range(r.randint(1, 200)))}


def s_fenced(r):
    inner = r.choice(["{\"a\": 1}", "{\"a\":", "```", "print(1)", "", "x" * 5000])
    return {"content": r.choice(["```json\n%s\n```", "```\n%s", "%s\n```", "```python\n%s\n```\n```json\n{}\n```"]) % inner}


def s_raw_body(r):
    bodies = [
        "", "null", "[]", "{}", "true", "12345", "\"str\"",
        "{\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"abc",
        "{\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"ok\"}]}}]} trailing",
        "<html><body>502 Bad Gateway</body></html>",
        "".join(chr(r.randint(0, 255)) for _ in range(r.randint(1, 400))),
        "[" * 20000 + "]" * 20000,
        "{\"a\":" * 5000 + "1" + "}" * 5000,
        "{\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"\\ud800\"}]}}]}",
        "\xef\xbb\xbf{\"candidates\": []}",
    ]
    return {"raw_body": r.choice(bodies), "status": r.choice([200, 200, 200, 500])}


def s_raw_payload(r):
    payloads = [
        {"candidates": []},
        {"candidates": [{}]},
        {"candidates": [{"content": {}}]},
        {"candidates": [{"content": {"parts": "notalist"}}]},
        {"candidates": [{"content": {"parts": [{"text": 5}]}}]},
        {"candidates": [{"content": {"parts": [{"text": None}]}}]},
        {"candidates": [{"content": {"parts": [{"functionCall": {"name": 7}}]}}]},
        {"candidates": [{"content": {"parts": [{"functionCall": {"args": "str"}}]}}]},
        {"candidates": [{"content": {"parts": [{"functionCall": {"name": "echo", "args": [1, 2]}}]}}]},
        {"candidates": [{"content": {"parts": [{"functionResponse": {"name": "echo"}}]}}]},
        {"candidates": [{"content": {"parts": [{"text": "a"}]}}, {"content": {"parts": [{"text": "b"}]}}]},
        {"candidates": [{"content": {"parts": [{"text": "ok"}]}, "finishReason": 42}],
         "usageMetadata": {"promptTokenCount": -5, "candidatesTokenCount": 2 ** 62, "thoughtsTokenCount": "x"}},
        {"error": {"code": 200, "message": "error inside a 200"}},
        {"candidates": [{"content": {"parts": [{"text": "ok"}]}}], "usageMetadata": None},
    ]
    return {"raw_payload": r.choice(payloads)}


def s_tool_calls(r):
    def call():
        name = r.choice(["echo", "echo", "echo", "nope", "", "e cho", "echo\u202e", "x" * 300,
                         "../echo", "echo;rm"])
        args = r.choice([{"text": "hi"}, {}, {"text": 5}, {"text": [1, 2]}, {"text": {"a": {"b": 1}}},
                         {"text": "y" * 200_000}, {"other": 1}, {"text": None}])
        return {"name": name, "args": args}
    calls = [call() for _ in range(r.choice([1, 1, 2, 5, 50]))]
    spec = {"tool_calls": calls}
    if r.random() < 0.3:
        spec["content"] = "and some text"
    return spec


def s_http_error(r):
    return {"status": r.choice([400, 401, 403, 404, 408, 429, 500, 502, 503]),
            "error": r.choice(["", "x" * 10_000, "\u202e", "quota", "{\"nested\": \"json\"}"])}


def s_secret_reply(r):
    key = "AK" + "IA" + "FUZZ" + "FUZZ" + "FUZZ" + "FUZZ"
    return {"content": "here is the key " + key}


def s_finish(r):
    return {"content": "partial answer", "finish_reason": r.choice(["MAX_TOKENS", "SAFETY", "RECITATION", "OTHER", "", "STOP"]),
            "thinking_tokens": r.choice([None, 0, 10 ** 9]), "output_tokens": r.choice([0, 1, 10 ** 9])}


def s_plain(r):
    return {"content": r.choice(["ok", "FINAL", "a reasonable answer about the task"])}


SHAPES = [s_empty, s_huge, s_unicode, s_fenced, s_raw_body, s_raw_payload, s_tool_calls,
          s_http_error, s_secret_reply, s_finish, s_plain]


def gen_case(seed):
    r = random.Random(seed)
    responses = [r.choice(SHAPES)(r) for _ in range(r.randint(1, 4))]
    responses.append({"content": "FINAL"})  # the last reply repeats once the list runs out
    contract = r.random() < 0.3
    return {"responses": responses, "validate_tool_pairing": True}, contract


# ---- one case ---------------------------------------------------------------

PROGRAM = """use agent

fn echo(text) {
    return "echo:" + string(text)
}

main {
    agent.register_tool("echo", echo, {
        "description": "echo the text back",
        "parameters": {"text": {"type": "string", "description": "text"}}
    })
    let h = agent.create("fz")
    let i = 0
    while i < 3 {
        try {
            let r = agent.send(h, "turn " + string(i))
            print("SENT " + string(i))
        } catch (e) {
            let why = string(e).split("\n")[0]
            print("CAUGHT " + string(i) + " " + why)
        }
        i = i + 1
    }
    print("FUZZ-DONE")
}
"""


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_stub(fixture, state_dir):
    for _ in range(5):
        port = free_port()
        p = subprocess.Popen([sys.executable, STUB, str(port), fixture, state_dir],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        line = p.stdout.readline().decode("ascii", "replace")
        if line.startswith("READY"):
            return p, port
        p.kill()
        p.wait()
    return None, None


def config(port, contract):
    agent = {
        "provider": "gemini", "model": "stub-model",
        "api_base": "http://127.0.0.1:%d" % port, "api_key_env": "FAKE_KEY_FUZZ",
        "max_tokens": 100, "max_turns": 50, "retry": {"max_attempts": 1},
        "tools_enabled": True, "tools": ["echo"],
        "max_tool_calls_per_turn": 5, "max_tool_loop_turns": 3,
    }
    if contract:
        agent["output_contract"] = {"format": "json", "required_fields": ["a"]}
    return {
        "version": "3.0", "mode": "enforce",
        "security": {"sandbox_level": "elevated"},
        "telemetry": {"enabled": True, "output_file": "telemetry.jsonl",
                      "tamper_evidence": {"enabled": True},
                      "transcript": {"enabled": True, "output_file": "transcript.jsonl"}},
        "agents": {"fz": agent},
    }


def run_case(naab, real_naab, seed, wdir, timeout, plant):
    os.makedirs(wdir, exist_ok=True)
    fixture, contract = gen_case(seed)
    fpath = os.path.join(wdir, "fixture.json")
    with open(fpath, "w", encoding="utf-8", errors="strict") as f:
        json.dump(fixture, f)
    stub, port = start_stub(fpath, wdir)
    if not stub:
        return "UNMEASURABLE", "the stub did not start", ""
    try:
        with open(os.path.join(wdir, "govern.json"), "w", encoding="utf-8", errors="strict") as f:
            json.dump(config(port, contract), f)
        with open(os.path.join(wdir, "t.naab"), "w", encoding="utf-8", errors="strict") as f:
            f.write(PROGRAM)
        env = dict(os.environ, FAKE_KEY_FUZZ="fake-key-fuzz", REAL_NAAB=real_naab,
                   STUB_PORT=str(port))
        t0 = time.monotonic()
        try:
            p = subprocess.run([naab, "--governance-report", "report.json", "t.naab"], cwd=wdir,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                               timeout=timeout)
            rc, text = p.returncode, p.stdout.decode("utf-8", "replace")
            with open(os.path.join(wdir, "out.txt"), "w", encoding="utf-8", errors="replace") as f:
                f.write(text)
        except subprocess.TimeoutExpired as e:
            return "FAIL", "hang: no exit within %ds" % timeout, (e.stdout or b"").decode("utf-8", "replace")
        ms = int((time.monotonic() - t0) * 1000)
    finally:
        stub.kill()
        stub.wait()
    tail = "\n".join(text.splitlines()[-12:])
    if rc < 0 or rc >= 128:
        return "FAIL", "crash: exit %d" % rc, tail
    if rc not in (0, 3, 4):
        return "FAIL", "exit %d (expected 0, 3 or 4)" % rc, tail
    if rc == 0 and "FUZZ-DONE" not in text:
        return "FAIL", "exit 0 without FUZZ-DONE: the run was cut short", tail
    pl = os.path.join(wdir, "pairing.log")
    if os.path.exists(pl) and os.path.getsize(pl) > 0:
        with open(pl, "rb") as f:
            return "FAIL", "pairing: the engine sent a request a provider rejects", f.read().decode("utf-8", "replace")[:400]
    if rc != 4:
        tel = os.path.join(wdir, "telemetry.jsonl")
        if not os.path.exists(tel) or os.path.getsize(tel) == 0:
            return "FAIL", "evidence: no telemetry written", tail
        v = subprocess.run([real_naab, "--verify-telemetry-chain", "telemetry.jsonl"], cwd=wdir,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        if v.returncode != 0:
            return "FAIL", "evidence: the telemetry chain does not verify (exit %d)" % v.returncode, \
                v.stdout.decode("utf-8", "replace")[-600:]
        rep = os.path.join(wdir, "report.json")
        try:
            with open(rep, "rb") as f:
                json.loads(f.read().decode("utf-8"))
        except (OSError, ValueError) as e:
            return "FAIL", "evidence: the governance report is missing or does not parse (%s)" % type(e).__name__, tail
    return "PASS", "exit %d in %d ms" % (rc, ms), tail


# ---- plants (controls) ----------------------------------------------------------

PLANTS = {
    "crash": "kill -SEGV $$\n",
    "hang": "sleep 1000\n",
    "exit1": "echo FUZZ-DONE; exit 1\n",
    "noevidence": "\"$REAL_NAAB\" \"$@\"; rc=$?; rm -f telemetry.jsonl; exit $rc\n",
    "pairing": (
        "python3 -c 'import json,sys,urllib.request\n"
        "b={\"contents\":[{\"role\":\"user\",\"parts\":[{\"functionResponse\":{\"name\":\"echo\",\"response\":{}}}]}]}\n"
        "r=urllib.request.Request(\"http://127.0.0.1:%s/v1beta/models/m:generateContent\" % sys.argv[1],"
        "data=json.dumps(b).encode(),headers={\"Content-Type\":\"application/json\"})\n"
        "try:\n urllib.request.build_opener(urllib.request.ProxyHandler({})).open(r,timeout=5)\n"
        "except Exception:\n pass' \"$STUB_PORT\"\n"
        "exec \"$REAL_NAAB\" \"$@\"\n"),
}


def make_plant(mode, d):
    path = os.path.join(d, "plant-" + mode)
    with open(path, "w", encoding="ascii", errors="strict") as f:
        f.write("#!/usr/bin/env bash\n" + PLANTS[mode])
    os.chmod(path, 0o755)
    return path


def parse_seeds(spec):
    seeds = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-", 1)
            seeds.extend(range(int(a), int(b) + 1))
        elif part:
            seeds.append(int(part))
    return seeds


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--naab", required=True)
    ap.add_argument("--seeds", default="1-40")
    ap.add_argument("--plant", choices=sorted(PLANTS))
    ap.add_argument("--keep")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--show", type=int, help="print the fixture a seed generates and exit")
    a = ap.parse_args(argv)
    if a.show is not None:
        fixture, contract = gen_case(a.show)
        text = json.dumps({"fixture": fixture, "output_contract": contract})
        out(text[:4000] + (" ... (%d chars)" % len(text) if len(text) > 4000 else ""))
        return 0
    real = os.path.abspath(a.naab)
    if not os.path.exists(real) and os.path.exists(real + ".exe"):
        real += ".exe"
    if not os.path.exists(real):
        out("UNMEASURABLE: --naab %s does not exist" % a.naab)
        return 1
    tmp = a.keep or tempfile.mkdtemp(prefix="naab_fuzz_mc_")
    naab = make_plant(a.plant, tmp) if a.plant else real
    counts = {"PASS": 0, "FAIL": 0, "UNMEASURABLE": 0}
    timeout = 8 if a.plant == "hang" else a.timeout
    for seed in parse_seeds(a.seeds):
        verdict, why, tail = run_case(naab, real, seed, os.path.join(tmp, "seed%d" % seed), timeout, a.plant)
        counts[verdict] += 1
        out("%s seed %d: %s" % (verdict, seed, why))
        if verdict != "PASS" and not a.plant:
            for line in tail.splitlines()[-8:]:
                out("       | " + line[:200])
            out("       | reproduce: %s --naab %s --seeds %d --keep DIR" % (os.path.relpath(__file__, REPO), a.naab, seed))
    if not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    out("cases: %d PASS, %d FAIL, %d UNMEASURABLE" % (counts["PASS"], counts["FAIL"], counts["UNMEASURABLE"]))
    if a.plant:
        return 0 if counts["FAIL"] > 0 and counts["PASS"] == 0 and counts["UNMEASURABLE"] == 0 else 1
    return 0 if counts["FAIL"] == 0 and counts["UNMEASURABLE"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
