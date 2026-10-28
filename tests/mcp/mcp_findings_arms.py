#!/usr/bin/env python3
"""Arms for tests/mcp/test_mcp_findings.sh -- what `naab-lang mcp` tells a
caller about a block (A43(b)(c)).

Usage: mcp_findings_arms.py NAAB WORKDIR

(b) naab_propose runs `naab-lang run --lint-only` in a worker and built its
    `findings` from the worker's STDERR only, while the CLI prints a HARD
    block on STDOUT: a blocked propose answered verdict "blocked", exit 3,
    findings [] -- the caller had no reason. It now carries the block, through
    the same secret/PII scan naab_run applies to stderr.
(c) `findings` kept the FIRST 60 non-empty stderr lines, so a program that
    wrote to stderr pushed out everything governance printed after it --
    including the end-of-run groups, printed last. It now keeps the head and
    the tail, and says how many lines it left out between them.

    MF-00   CONTROL: the server answers initialize, and a clean propose passes
            with a token (the fixture and the transport work)
    MF-01   a blocked propose (a TODO under no_placeholders) carries the
            block's header and its rule in `findings`
    MF-02   a blocked propose of a secret carries the rule, never the secret
            (the scan the findings go through)
    MF-02c  CONTROL: the secret program runs without the no_secrets rule --
            MF-02's verdict is the policy's
    MF-03   naab_run of a program that writes 80 stderr lines and then trips
            an undeclared-effects advisory: `findings` carries the end-of-run
            group and says lines were omitted
    MF-03c  CONTROL: the same program without the 80 lines -- the group is
            printed at all under this policy
    MF-04   propose's new stdout channel goes through the scan: a parse error
            quotes its source line, and an SSN written as bare arithmetic is not
            a string literal, so the CLI prints it raw on stdout. Under no_pii
            the propose findings are withheld and the value appears nowhere
    MF-04c  CONTROL: without no_pii the same propose carries that line -- the
            channel delivers it, so MF-04's absence is the scan's

Prints PASS / FAIL / SKIP (UNMEASURABLE) lines; exits with the FAIL count.
"""
import json
import os
import subprocess
import sys

NAAB = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(sys.argv[2])
RESULTS = []
KEY = "AK" + "IA" + "TEST" * 4      # secret-SHAPED, assembled at run time
SSN = "12" + "3-45-" + "6789"       # PII-SHAPED, assembled at run time


def report(status, arm, msg, ev=""):
    RESULTS.append(status)
    line = "%s [%s] %s" % (status, arm, msg)
    if ev:
        line += "\n       -> " + ev.replace("\n", " | ")[:700]
    print(line, flush=True)


def project(name, extra):
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    cfg = {"mode": "enforce", "security": {"sandbox_level": "elevated"}}
    cfg.update(extra)
    with open(os.path.join(d, "govern.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f)
    return d


class Server:
    def __init__(self, cwd):
        self.p = subprocess.Popen([NAAB, "mcp"], cwd=cwd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.next_id = 1

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        try:
            self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                                            "params": params}) + "\n").encode())
            self.p.stdin.flush()
            for raw in self.p.stdout:
                msg = json.loads(raw)
                if msg.get("id") == rid:
                    return msg
        except (OSError, ValueError):
            pass               # a server that died or spoke no JSON-RPC
        return None

    def notify(self, method):
        try:
            self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "method": method}) + "\n").encode())
            self.p.stdin.flush()
        except OSError:
            pass

    def init(self):
        r = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                        "clientInfo": {"name": "naab-mf-test", "version": "1"}})
        self.notify("notifications/initialized")
        return r

    def call(self, tool, code):
        r = self.request("tools/call", {"name": tool, "arguments": {"code": code}})
        return (r or {}).get("result", {}).get("structuredContent") or {}

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


def brief(st):
    return json.dumps(st)[:700]


TODO_PROG = "fn f() {\n    // TODO: finish\n    return 1\n}\nmain {\n    print(f())\n}\n"
SECRET_PROG = 'main {\n    let k = "%s"\n    print(k)\n}\n' % KEY
NOISE = ("fn noisy() {\n    let i = 0\n    while i < 80 {\n"
         "        io.write_error(\"noise \" + string(i) + \"\\n\")\n        i = i + 1\n    }\n}\n")
EFFECT = 'fn alpha() { file.write("a.txt", "x") }\n'
NOISY_PROG = "use io\nuse file\n" + NOISE + EFFECT + "main {\n    noisy()\n    alpha()\n    print(\"DONE\")\n}\n"
QUIET_PROG = "use file\n" + EFFECT + "main {\n    alpha()\n    print(\"DONE\")\n}\n"
GROUP = "Undeclared function effects"
PARSE_PROG = "main {\n    let x = %s +* 1\n    print(x)\n}\n" % SSN


def main():
    os.makedirs(WORK, exist_ok=True)
    p_todo = project("todo", {"code_quality": {"no_placeholders": {"enabled": True, "level": "hard"}}})
    p_secret = project("secret", {"code_quality": {"no_secrets": {"enabled": True, "level": "hard"}}})
    p_plain = project("plain", {})
    p_fn = project("fn", {"capabilities": {"filesystem": {"mode": "readwrite"},
                                           "functions": {"default": {"allowed_actions": ["FS_READ"]}}}})

    s = Server(p_todo)
    r = s.init()
    st = s.call("naab_propose", 'main {\n    print("ok")\n}\n') if r and "result" in r else {}
    if st.get("verdict") == "pass" and st.get("token"):
        report("PASS", "MF-00", "CONTROL: the server answers, and a clean propose passes with a token")
        live = True
    else:
        report("SKIP", "MF-00", "UNMEASURABLE: no server or no clean propose", brief(st) if st else str(r))
        live = False
    if not live:
        s.close()
        return finish()

    st = s.call("naab_propose", TODO_PROG)
    f = "\n".join(x for x in st.get("findings", []) if isinstance(x, str))
    if (st.get("verdict") == "blocked" and not st.get("token")
            and 'Placeholder "TODO" found in code' in f and "code_quality.no_placeholders" in f):
        report("PASS", "MF-01", "a blocked propose carries the block and its rule in findings")
    else:
        report("FAIL", "MF-01", "a blocked propose should say why", brief(st))
    s.close()

    s = Server(p_secret)
    s.init()
    st = s.call("naab_propose", SECRET_PROG)
    f = "\n".join(x for x in st.get("findings", []) if isinstance(x, str))
    if st.get("verdict") == "blocked" and "code_quality.no_secrets" in f and KEY not in json.dumps(st):
        report("PASS", "MF-02", "a blocked secret propose names the rule and never carries the secret")
    else:
        report("FAIL", "MF-02", "a blocked secret propose should name the rule without the secret", brief(st))
    s.close()

    s = Server(p_plain)
    s.init()
    st = s.call("naab_propose", SECRET_PROG)
    if st.get("verdict") == "pass":
        report("PASS", "MF-02c", "CONTROL: without no_secrets the same program passes propose")
    else:
        report("FAIL", "MF-02c", "the secret program should pass without the rule", brief(st))
    s.close()

    s = Server(p_fn)
    s.init()
    quiet = s.call("naab_run", QUIET_PROG)
    qf = "\n".join(x for x in quiet.get("findings", []) if isinstance(x, str))
    if quiet.get("verdict") == "pass" and GROUP in qf:
        report("PASS", "MF-03c", "CONTROL: the undeclared-effects group reaches findings when nothing precedes it")
        st = s.call("naab_run", NOISY_PROG)
        f = "\n".join(x for x in st.get("findings", []) if isinstance(x, str))
        if st.get("verdict") == "pass" and GROUP in f and "lines omitted" in f and "DONE" in st.get("output", ""):
            report("PASS", "MF-03", "after 80 stderr lines, findings still carry the end-of-run group and mark the gap")
        else:
            report("FAIL", "MF-03", "the end-of-run group should survive 80 lines of program stderr", brief(st))
    else:
        report("FAIL", "MF-03c", "the undeclared-effects group should reach findings", brief(quiet))
        report("SKIP", "MF-03", "UNMEASURABLE: MF-03c's group never appeared")
    s.close()

    for arm, proj, want in (("MF-04c", project("pii_off", {}), True),
                            ("MF-04", project("pii_on", {"code_quality": {"no_pii": {"enabled": True, "level": "hard"}}}), False)):
        s = Server(proj)
        s.init()
        st = s.call("naab_propose", PARSE_PROG)
        s.close()
        carried = SSN in "\n".join(x for x in st.get("findings", []) if isinstance(x, str))
        anywhere = SSN in json.dumps(st)
        if want and carried:
            report("PASS", arm, "CONTROL: without no_pii the parse error's source line reaches propose findings")
        elif not want and not anywhere and st.get("verdict") == "blocked" and not st.get("token") \
                and any("diagnostics withheld" in x for x in st.get("findings", []) if isinstance(x, str)):
            report("PASS", arm, "under no_pii the propose findings are withheld; the value appears nowhere")
        else:
            report("FAIL", arm, "the propose findings channel %s" % ("should carry the line" if want else "should be scanned"),
                   brief(st).replace(SSN, "<SSN>"))
    return finish()


def finish():
    fails = RESULTS.count("FAIL")
    print("MF: %d pass, %d fail, %d unmeasurable" % (RESULTS.count("PASS"), fails, RESULTS.count("SKIP")),
          flush=True)
    return min(fails, 100)


if __name__ == "__main__":
    sys.exit(main())
