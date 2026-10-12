#!/usr/bin/env python3
"""Arms for tests/mcp/test_mcp_stdio.sh -- `naab-lang mcp` over stdio.

Standard library only. Every refusal is paired with a success through the same
fixture, because a refusal-expecting arm passes for free whenever the fixture
is broken. Each arm prints one line:

    PASS [MC-xx] what held
    FAIL [MC-xx] what did not  -> evidence
    SKIP [MC-xx] UNMEASURABLE: why the arm could not run

The exit status is the number of FAILs (capped at 100). SKIP is not a pass.

Usage: mcp_stdio_arms.py NAAB WORKDIR
"""
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time

NAAB = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(sys.argv[2])

RESULTS = []          # (status, arm, message)
# A secret-SHAPED value (AWS's documentation example key), assembled at run
# time so no secret-shaped literal sits in this file for a scanner to flag.
AWS_EXAMPLE = "AKIA" + "IOSFODNN7" + "EXAMPLE"
V2025 = "2025-11-25"
V2026 = "2026-07-28"
TOOLS = {"naab_lease", "naab_propose", "naab_commit", "naab_run", "naab_verify_attestation"}


def report(status, arm, msg, evidence=""):
    RESULTS.append((status, arm, msg))
    line = "%s [%s] %s" % (status, arm, msg)
    if evidence:
        line += "\n       -> " + evidence.replace("\n", " | ")[:600]
    print(line, flush=True)


def ok(arm, msg): report("PASS", arm, msg)
def bad(arm, msg, ev=""): report("FAIL", arm, msg, ev)
def unmeasurable(arm, msg): report("SKIP", arm, "UNMEASURABLE: " + msg)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

BSD_PATTERN = {
    "enabled": True,
    "patterns": [{
        "name": "mcp_split_exfil", "level": "hard", "max_gap": 20,
        "decay_seconds": 600,
        "sequence": ["env.get:*MCP_TEST_SECRET*", "encode", "file.write:*outbox*"],
    }],
}


def project(name, extra=None):
    """A project directory with its own govern.json; returns its path."""
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    cfg = {"version": "5.0", "mode": "enforce",
           "security": {"sandbox_level": "elevated"}}
    for k, v in (extra or {}).items():
        cfg[k] = v
    with open(os.path.join(d, "govern.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    return d


def prog(body, uses=()):
    head = "".join("use %s\n" % u for u in uses)
    return head + "main {\n" + body + "\n}\n"


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

class Server:
    """One `naab-lang mcp` process, i.e. one caller's session."""

    def __init__(self, cwd, env_extra=None):
        env = dict(os.environ)
        env.update(env_extra or {})
        self.errpath = os.path.join(cwd, ".mcp-server-%d.err" % time.time_ns())
        self.err = open(self.errpath, "wb")
        self.p = subprocess.Popen([NAAB, "mcp"], cwd=cwd, env=env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err)
        self.responses = {}
        self.notes = []        # (arrival_seq, msg)
        self.seq = 0
        self.cv = threading.Condition()
        self.next_id = 1
        self.raw_lines = []
        self.eof = False
        self.lock = threading.Lock()
        t = threading.Thread(target=self._read, daemon=True)
        t.start()

    def _read(self):
        for line in self.p.stdout:
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            with self.cv:
                self.raw_lines.append(text)
                self.seq += 1
                try:
                    msg = json.loads(text)
                except Exception:
                    msg = {"__unparseable__": text}
                if isinstance(msg, dict) and "id" in msg and ("result" in msg or "error" in msg):
                    msg["__seq__"] = self.seq
                    self.responses[json.dumps(msg["id"])] = msg
                else:
                    self.notes.append((self.seq, msg))
                self.cv.notify_all()
        with self.cv:
            self.eof = True
            self.cv.notify_all()

    def send(self, obj):
        self.send_raw(json.dumps(obj))

    def send_raw(self, text):
        # A server that is not there (not built, crashed, wrong subcommand)
        # must read as a failed arm, not kill the client.
        try:
            with self.lock:
                self.p.stdin.write(text.encode() + b"\n")
                self.p.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            with self.cv:
                self.eof = True
                self.cv.notify_all()

    def start(self, method, params=None, rid=None):
        if rid is None:
            rid = self.next_id
            self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return rid

    def wait(self, rid, timeout=60.0):
        key = json.dumps(rid)
        deadline = time.time() + timeout
        with self.cv:
            while key not in self.responses:
                left = deadline - time.time()
                if left <= 0 or self.eof:
                    return None
                self.cv.wait(min(left, 0.2))
            return self.responses[key]

    def request(self, method, params=None, timeout=60.0, rid=None):
        return self.wait(self.start(method, params, rid), timeout)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def init2025(self):
        r = self.request("initialize", {
            "protocolVersion": V2025, "capabilities": {},
            "clientInfo": {"name": "naab-mcp-test", "version": "1"}}, timeout=20)
        self.notify("notifications/initialized")
        return r

    def call(self, name, args, timeout=90.0, progress=None, rid=None):
        params = {"name": name, "arguments": args}
        if progress is not None:
            params["_meta"] = {"progressToken": progress}
        r = self.request("tools/call", params, timeout=timeout, rid=rid)
        return r

    def alive(self):
        return self.p.poll() is None

    def children(self):
        """PIDs whose parent is this server (Linux /proc)."""
        out = []
        for e in os.listdir("/proc"):
            if not e.isdigit():
                continue
            try:
                with open("/proc/%s/stat" % e, "rb") as f:
                    stat = f.read().decode("utf-8", "replace")
                ppid = int(stat.rsplit(")", 1)[1].split()[1])
                state = stat.rsplit(")", 1)[1].split()[0]
            except Exception:
                continue
            if ppid == self.p.pid and state != "Z":
                out.append(int(e))
        return out

    def stderr_text(self):
        try:
            self.err.flush()
            with open(self.errpath, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except Exception:
            return ""

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(10)
        except Exception:
            self.p.kill()
            self.p.wait()
        self.err.close()


def sc(resp):
    """structuredContent of a tools/call response, or {}."""
    if not resp or "result" not in resp:
        return {}
    return resp["result"].get("structuredContent") or {}


def brief(resp):
    if resp is None:
        return "no response"
    s = json.dumps(resp)
    return s[:400]


def run(srv, code, **kw):
    args = {"code": code}
    if "lease" in kw:
        args["lease_id"] = kw.pop("lease")
    return srv.call("naab_run", args, **kw)


# ---------------------------------------------------------------------------
# The sequence's last step writes an "outbox" file: an observable, local sink.
# (A loopback HTTP stub cannot serve here: the http module refuses private and
# loopback addresses unconditionally, as SSRF protection.)
# ---------------------------------------------------------------------------

def outbox_in(project_dir):
    return os.path.join(project_dir, "outbox.txt")


def clear_outbox(project_dir):
    try:
        os.remove(outbox_in(project_dir))
    except FileNotFoundError:
        pass


def outbox_written(project_dir):
    return os.path.exists(outbox_in(project_dir))


# ---------------------------------------------------------------------------
# Arms
# ---------------------------------------------------------------------------

def main():
    os.makedirs(WORK, exist_ok=True)
    secret_env = {"MCP_TEST_SECRET": "not-a-real-secret"}

    p_main = project("p_main", {"behavioral_sequences": BSD_PATTERN,
                                "code_quality": {"no_secrets": "hard"}})
    p_noscan = project("p_noscan", {"behavioral_sequences": BSD_PATTERN})
    p_noshell = project("p_noshell", {"capabilities": {"shell": {"enabled": False}}})
    p_nobsd = project("p_nobsd")

    # ---- MC-00: 2025-11-25 handshake + tools/list ---------------------------
    s = Server(p_main, secret_env)
    r = s.init2025()
    if r is None or "result" not in r:
        bad("MC-00", "no initialize result -- server not built or transport broken",
            brief(r) + " stderr=" + s.stderr_text()[-300:])
        s.close()
        # Nothing below can be measured without a server.
        for a in ("MC-01", "MC-02", "MC-03", "MC-04", "MC-05", "MC-06", "MC-07",
                  "MC-08", "MC-09", "MC-10", "MC-11", "MC-12", "MC-13", "MC-14",
                  "MC-15", "MC-17", "MC-18", "MC-19"):
            unmeasurable(a, "MC-00 failed: no server to test")
        return finish()
    res = r["result"]
    tl = s.request("tools/list", {})
    names = {t.get("name") for t in (tl or {}).get("result", {}).get("tools", [])}
    schemas_ok = all(isinstance(t.get("inputSchema"), dict) and t["inputSchema"].get("type") == "object"
                     for t in (tl or {}).get("result", {}).get("tools", []))
    if (res.get("protocolVersion") == V2025 and "tools" in res.get("capabilities", {})
            and res.get("serverInfo", {}).get("name") and TOOLS <= names and schemas_ok):
        ok("MC-00", "initialize negotiates %s; tools/list lists %s" % (V2025, sorted(TOOLS)))
    else:
        bad("MC-00", "handshake or tool list wrong", brief(r) + " tools=" + str(sorted(names)))

    # ---- MC-02: positive control for everything below ----------------------
    r = run(s, prog('    print("MC02_OK")'))
    st = sc(r)
    control_ok = st.get("verdict") == "pass" and "MC02_OK" in st.get("output", "")
    if control_ok:
        ok("MC-02", "naab_run returns the program's output (verdict pass)")
    else:
        bad("MC-02", "a trivial program did not run -- every arm below is suspect", brief(r))

    # ---- MC-03: propose runs nothing; a violation yields no token ------------
    r = s.call("naab_propose", {"code": prog('    let k = "%s"\n    print(k)' % AWS_EXAMPLE)})
    st = sc(r)
    a = st.get("verdict") == "blocked" and not st.get("token")
    marker = os.path.join(p_main, "mc03_marker.txt")
    clean = prog('    file.write("mc03_marker.txt", "ran")\n    print("MC03")', uses=("file",))
    r2 = s.call("naab_propose", {"code": clean})
    st2 = sc(r2)
    b = st2.get("verdict") == "pass" and bool(st2.get("token")) and not os.path.exists(marker)
    r3 = s.call("naab_commit", {"token": st2.get("token", "")}) if st2.get("token") else None
    st3 = sc(r3)
    c = st3.get("verdict") == "pass" and os.path.exists(marker)
    if a and b and c:
        ok("MC-03", "a violation is blocked with no token; a clean propose writes nothing; its commit writes the marker")
    else:
        bad("MC-03", "propose/commit contract broken (blocked-no-token=%s, propose-ran-nothing=%s, commit-ran=%s)" % (a, b, c),
            brief(r) + " || " + brief(r2) + " || " + brief(r3))

    # ---- MC-04: tokens are single-use --------------------------------------
    if st2.get("token") and c:
        r4 = s.call("naab_commit", {"token": st2["token"]})
        st4 = sc(r4)
        if r4 and r4.get("result", {}).get("isError") and st4.get("verdict") == "refused":
            ok("MC-04", "a replayed token is refused")
        else:
            bad("MC-04", "a replayed token was accepted", brief(r4))
    else:
        unmeasurable("MC-04", "no committed token from MC-03")

    # ---- MC-05: token bound to the policy -----------------------------------
    r = s.call("naab_propose", {"code": prog('    print("MC05")')})
    tok = sc(r).get("token")
    gpath = os.path.join(p_main, "govern.json")
    with open(gpath, encoding="utf-8") as f:
        original = f.read()
    if tok:
        cfg = json.loads(original)
        cfg["_comment_mcp"] = "changed after propose"
        with open(gpath, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
        try:
            r5 = s.call("naab_commit", {"token": tok})
        finally:
            with open(gpath, "w", encoding="utf-8") as f:
                f.write(original)
        st5 = sc(r5)
        if st5.get("verdict") == "refused" and "MC05" not in st5.get("output", ""):
            ok("MC-05", "commit after a policy change is refused (MC-03's unchanged commit is the control)")
        else:
            bad("MC-05", "commit ran under a policy it was not checked against", brief(r5))
    else:
        unmeasurable("MC-05", "propose of a clean program returned no token: " + brief(r))

    # ---- MC-06: a lease narrows; the same program runs with the grant -------
    rl = s.call("naab_lease", {"capabilities": ["FS_READ"]})
    lease_ro = sc(rl).get("lease_id")
    rl2 = s.call("naab_lease", {"capabilities": ["FS_READ", "FS_WRITE"]})
    lease_rw = sc(rl2).get("lease_id")
    out6 = os.path.join(p_main, "mc06.txt")
    w = prog('    file.write("mc06.txt", "x")\n    print("MC06")', uses=("file",))
    if lease_ro and lease_rw:
        ra = run(s, w, lease=lease_ro)
        refused = sc(ra).get("verdict") == "blocked" and not os.path.exists(out6)
        rb = run(s, w, lease=lease_rw)
        allowed = sc(rb).get("verdict") == "pass" and os.path.exists(out6)
        if refused and allowed:
            ok("MC-06", "a write is refused without FS_WRITE and runs with it")
        else:
            bad("MC-06", "lease not enforced (refused=%s, allowed=%s)" % (refused, allowed),
                brief(ra) + " || " + brief(rb))
        # The sandbox half. A <<sh>> block is not the test: it passes through
        # checkShellAllowed(), which consults the lease helper too, so it stays
        # refused with the sandbox narrowing removed (the mutation run showed
        # it). Embedded Python's own open() and os.system() reach no
        # capability helper -- only the sandbox, synced from the narrowed
        # capability switches.
        l_rw = sc(s.call("naab_lease", {"capabilities": ["FS_READ", "FS_WRITE"]})).get("lease_id")
        l_ro = sc(s.call("naab_lease", {"capabilities": ["FS_READ"]})).get("lease_id")
        l_rwx = sc(s.call("naab_lease", {"capabilities": ["FS_READ", "FS_WRITE", "SHELL_EXEC"]})).get("lease_id")
        pyw = prog('    let r = <<python\nopen("mc06p.txt", "w").write("x")\n"wrote"\n>>\n    print(r)')
        pysh = prog('    let r = <<python\nimport os\nos.system("touch mc06x.txt")\n"ran"\n>>\n    print(r)')
        fw = os.path.join(p_main, "mc06p.txt")
        fx = os.path.join(p_main, "mc06x.txt")
        w_ok = sc(run(s, pyw, lease=l_rw)).get("verdict") == "pass" and os.path.exists(fw)
        x_ok = sc(run(s, pysh, lease=l_rwx)).get("verdict") == "pass" and os.path.exists(fx)
        for f in (fw, fx):
            if os.path.exists(f):
                os.remove(f)
        if not (w_ok and x_ok):
            unmeasurable("MC-06s", "control: embedded python did not write / run a command even with the grant (write=%s exec=%s)" % (w_ok, x_ok))
        else:
            rw_no = run(s, pyw, lease=l_ro)
            rx_no = run(s, pysh, lease=l_rw)
            w_refused = sc(rw_no).get("verdict") != "pass" and not os.path.exists(fw)
            x_refused = sc(rx_no).get("verdict") != "pass" and not os.path.exists(fx)
            if w_refused and x_refused:
                ok("MC-06s", "embedded python's open() and os.system() are refused without FS_WRITE / SHELL_EXEC and run with them")
            else:
                bad("MC-06s", "the lease did not reach the sandbox (python write refused=%s, os.system refused=%s)" % (w_refused, x_refused),
                    brief(rw_no) + " || " + brief(rx_no))
    else:
        bad("MC-06", "naab_lease returned no lease_id", brief(rl) + " || " + brief(rl2))

    # ---- MC-06a: AGENT_SEND, which only the capability helper sees ---------
    # Narrowing the capability switches (MC-06/06s) cannot express "no model
    # calls"; the lease's intersection in checkFunctionCapability() does.
    stub_dir = os.path.join(WORK, "agent_stub")
    os.makedirs(stub_dir, exist_ok=True)
    with open(os.path.join(stub_dir, "fixture.json"), "w") as f:
        json.dump({"responses": [{"content": "STUB_REPLY_MC06A", "output_tokens": 5}]}, f)
    import socket
    sock = socket.socket(); sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]; sock.close()
    stub_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "helpers", "agent_stub.py")
    stub = subprocess.Popen([sys.executable, stub_py, str(port), os.path.join(stub_dir, "fixture.json"), stub_dir],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    up = False
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); up = True; break
        except OSError:
            time.sleep(0.05)
    p_agent = project("p_agent", {"agents": {"helper": {
        "provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%d" % port,
        "api_key_env": "FAKE_KEY_MCP_TEST", "max_tokens": 256, "max_turns": 5,
        "system_prompt": "Answer briefly."}}})
    agent_prog = prog('    let h = agent.create("helper")\n    let r = agent.send(h, "hi")\n'
                      '    print("AGENT=" + r.get("content"))', uses=("agent",))
    sa6 = Server(p_agent, {"FAKE_KEY_MCP_TEST": "not-a-real-key"}); sa6.init2025()
    l_with = sc(sa6.call("naab_lease", {"capabilities": ["AGENT_SEND", "NET_CONNECT"]})).get("lease_id")
    l_without = sc(sa6.call("naab_lease", {"capabilities": ["NET_CONNECT"]})).get("lease_id")
    rw = run(sa6, agent_prog, lease=l_with)
    rwo = run(sa6, agent_prog, lease=l_without)
    sa6.close()
    stub.kill(); stub.wait()
    if not up or "STUB_REPLY_MC06A" not in sc(rw).get("output", ""):
        unmeasurable("MC-06a", "control: agent.send did not reach the stub even with AGENT_SEND: " + brief(rw))
    elif sc(rwo).get("verdict") == "blocked" and "STUB_REPLY_MC06A" not in sc(rwo).get("output", ""):
        ok("MC-06a", "agent.send is refused without AGENT_SEND and reaches the provider with it")
    else:
        bad("MC-06a", "a lease without AGENT_SEND let a model call through", brief(rwo))

    # ---- MC-07: a lease cannot widen ---------------------------------------
    sn = Server(p_noshell)
    sn.init2025()
    rn = sn.call("naab_lease", {"capabilities": ["SHELL_EXEC", "FS_READ"]})
    stn = sc(rn)
    denied = {d.get("capability") for d in stn.get("denied", [])}
    rc = s.call("naab_lease", {"capabilities": ["SHELL_EXEC", "FS_READ"]})
    ctl = "SHELL_EXEC" in sc(rc).get("granted", [])
    bad_name = sn.call("naab_lease", {"capabilities": ["FLY_TO_MOON"]})
    bad_rejected = bad_name is not None and "error" in bad_name and bad_name["error"].get("code") == -32602
    if "SHELL_EXEC" in denied and "SHELL_EXEC" not in stn.get("granted", []) and "FS_READ" in stn.get("granted", []) and ctl and bad_rejected:
        ok("MC-07", "a capability the policy denies is listed in denied, not granted (granted in a project that allows it); an unknown name is -32602")
    else:
        bad("MC-07", "lease widened or mis-reported (control granted=%s, unknown-name rejected=%s)" % (ctl, bad_rejected),
            brief(rn) + " || " + brief(bad_name))
    sn.close()

    # ---- MC-08: no state crosses calls --------------------------------------
    set_py = prog('    let a = <<python\nmc08_mark = "TOKEN_PY"\n"set"\n>>\n    print(a)')
    get_py = prog('    let b = <<python\nglobals().get("mc08_mark", "ABSENT")\n>>\n    print("PY=" + b)')
    both = prog('    let a = <<python\nmc08_mark = "TOKEN_PY"\n"set"\n>>\n'
                '    let b = <<python\nglobals().get("mc08_mark", "ABSENT")\n>>\n    print("PY=" + b)')
    rc = run(s, both)
    python_ok = "PY=TOKEN_PY" in sc(rc).get("output", "")
    if not python_ok:
        unmeasurable("MC-08", "two python blocks in one call do not share state here (no embedded python?): " + brief(rc))
    else:
        run(s, set_py)
        rg = run(s, get_py)
        if "PY=ABSENT" in sc(rg).get("output", ""):
            ok("MC-08", "a python global set in one call is ABSENT in the next (one call shares it: control)")
        else:
            bad("MC-08", "state crossed from one call to the next", brief(rg))

    # ---- MC-09: a crash fails alone -----------------------------------------
    crash = prog('    let c = <<python\nimport ctypes\nctypes.string_at(0)\n>>\n    print(c)')
    if python_ok:
        rx = run(s, crash)
        crashed = sc(rx).get("verdict") in ("crashed", "error") and rx is not None
        ry = run(s, prog('    print("MC09_AFTER")'))
        if crashed and s.alive() and "MC09_AFTER" in sc(ry).get("output", ""):
            ok("MC-09", "a crashing call fails alone (verdict %s); the next call runs" % sc(rx).get("verdict"))
        else:
            bad("MC-09", "a crash reached the server or the next call", brief(rx) + " || " + brief(ry))
    else:
        unmeasurable("MC-09", "no embedded python to crash")
    s.close()

    # ---- MC-10 / MC-11: sequence split across calls --------------------------
    read_c = prog('    let k = env.get("MCP_TEST_SECRET") ?? "none"\n    print("C1")', uses=("env",))
    noise_c = prog('    print("C2")')
    enc_c = prog('    let e = crypto.base64_encode("abc")\n    print("C3")', uses=("crypto",))
    send_c = prog('    file.write("outbox.txt", "YWJj")\n    print("C4")', uses=("file",))
    whole = prog('    let k = env.get("MCP_TEST_SECRET") ?? "none"\n'
                 '    let e = crypto.base64_encode("abc")\n'
                 '    file.write("outbox.txt", e)\n    print("WHOLE")',
                 uses=("env", "crypto", "file"))

    # control A: the send alone writes the outbox (the path works at all)
    sa = Server(p_noscan, secret_env); sa.init2025()
    clear_outbox(p_noscan)
    ra = run(sa, send_c)
    send_works = sc(ra).get("verdict") == "pass" and outbox_written(p_noscan)
    sa.close()
    # control B: the whole sequence in one call is blocked
    sb = Server(p_noscan, secret_env); sb.init2025()
    clear_outbox(p_noscan)
    rb = run(sb, whole)
    one_call_blocks = sc(rb).get("verdict") == "blocked" and not outbox_written(p_noscan)
    sb.close()
    if not (send_works and one_call_blocks):
        unmeasurable("MC-10", "controls failed (send alone writes the outbox=%s, one-call sequence blocked=%s): %s || %s"
                     % (send_works, one_call_blocks, brief(ra), brief(rb)))
        unmeasurable("MC-11", "MC-10 controls failed")
    else:
        s2 = Server(p_noscan, secret_env); s2.init2025()
        v = [sc(run(s2, c)).get("verdict") for c in (read_c, noise_c, enc_c)]
        clear_outbox(p_noscan)
        r4 = run(s2, send_c)
        if v == ["pass", "pass", "pass"] and sc(r4).get("verdict") == "blocked" and not outbox_written(p_noscan):
            ok("MC-10", "read, unrelated, encode, send across four calls: the send is blocked before it leaves")
        else:
            bad("MC-10", "cross-call sequence not caught (earlier verdicts %s)" % v, brief(r4))
        clear_outbox(p_noscan)
        r5 = run(s2, send_c)
        if sc(r5).get("verdict") == "pass" and outbox_written(p_noscan):
            ok("MC-11", "after the block, the same send alone runs: history is not charged twice")
        else:
            bad("MC-11", "replayed history re-enforced a sequence already blocked", brief(r5))
        s2.close()

    # ---- MC-12: cancellation kills the worker --------------------------------
    s3 = Server(p_nobsd); s3.init2025()
    long_p = prog('    time.sleep(30)\n    file.write("mc12_after.txt", "x")', uses=("time", "file"))
    rid = s3.start("tools/call", {"name": "naab_run", "arguments": {"code": long_p}})
    t0 = time.time()
    kids = []
    while time.time() - t0 < 10 and not kids:
        kids = s3.children()
        time.sleep(0.05)
    if not kids:
        unmeasurable("MC-12", "no worker process appeared under the server")
    else:
        time.sleep(0.5)
        s3.notify("notifications/cancelled", {"requestId": rid, "reason": "test"})
        t1 = time.time()
        while time.time() - t1 < 3 and any(os.path.exists("/proc/%d" % k) and k in s3.children() for k in kids):
            time.sleep(0.02)
        gone_in = time.time() - t1
        still = [k for k in kids if k in s3.children()]
        rp = run(s3, prog('    print("MC12_NEXT")'))
        if not still and gone_in < 1.0 and "MC12_NEXT" in sc(rp).get("output", "") \
                and not os.path.exists(os.path.join(p_nobsd, "mc12_after.txt")):
            ok("MC-12", "a cancelled worker is gone in %.2fs and the next call runs" % gone_in)
        else:
            bad("MC-12", "cancel did not kill the worker (left=%s, %.2fs)" % (still, gone_in), brief(rp))
    s3.close()

    # ---- MC-13: progress notifications precede the result --------------------
    s4 = Server(p_noscan, secret_env); s4.init2025()
    prog13 = prog('    let k = env.get("MCP_TEST_SECRET") ?? "none"\n'
                  '    let e = crypto.base64_encode("abc")\n    print("MC13")', uses=("env", "crypto"))
    r = s4.call("naab_run", {"code": prog13}, progress="pt-13")
    rseq = (r or {}).get("__seq__", 0)
    prog_notes = [(q, m) for q, m in s4.notes
                  if m.get("method") == "notifications/progress"
                  and m.get("params", {}).get("progressToken") == "pt-13"]
    n_before = len(prog_notes)
    s4.notes.clear()
    r_no = s4.call("naab_run", {"code": prog13})
    none_without = not any(m.get("method") == "notifications/progress" for _, m in s4.notes)
    leaked = [m for _, m in prog_notes if "abc" in json.dumps(m) or "MCP_TEST_SECRET" in json.dumps(m)]
    if leaked:
        bad("MC-13d", "a progress notification carried an event detail (an argument value)", json.dumps(leaked[0])[:300])
    elif prog_notes:
        ok("MC-13d", "progress notifications carry event types, not argument values")
    if prog_notes and all(q < rseq for q, _ in prog_notes) and none_without and sc(r).get("verdict") == "pass":
        ok("MC-13", "%d progress notification(s) arrive before the result; none without a progressToken" % n_before)
    else:
        bad("MC-13", "progress missing, late, or unsolicited (n=%d, none_without=%s)" % (n_before, none_without), brief(r))
    s4.close()

    # ---- MC-14: attestations -------------------------------------------------
    key = os.path.join(WORK, "attest_key.pem")
    subprocess.run([NAAB, "--keygen", key], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p_sign = project("p_sign", {"audit": {"provenance": {"sign_records": True, "signing_key": key}}})
    with open(os.path.join(p_sign, "other.txt"), "w") as f:
        f.write("readable")
    s5 = Server(p_sign); s5.init2025()
    r1 = run(s5, prog('    print("MC14A")'))
    r2 = run(s5, prog('    print("MC14B")'))
    a1 = sc(r1).get("attestation") or {}
    a2 = sc(r2).get("attestation") or {}
    v1 = sc(s5.call("naab_verify_attestation", {"attestation": a1}))
    forged = json.loads(json.dumps(a1))
    if isinstance(forged.get("exit_code"), int):
        forged["exit_code"] = forged["exit_code"] + 1
    v2 = sc(s5.call("naab_verify_attestation", {"attestation": forged}))
    canon = json.dumps(a1, sort_keys=True, separators=(",", ":"))
    chain_ok = a2.get("prev_attestation_sha256") == hashlib.sha256(canon.encode()).hexdigest()
    if a1.get("signed") is True and v1.get("valid") is True and v2.get("valid") is False and chain_ok:
        ok("MC-14", "attestations are signed, verify, fail when one field changes, and chain")
    else:
        bad("MC-14", "attestation contract broken (signed=%s valid=%s forged_valid=%s chain=%s)"
            % (a1.get("signed"), v1.get("valid"), v2.get("valid"), chain_ok), json.dumps(a1)[:300])
    # The key must not be readable by the program it attests.
    rk = run(s5, prog('    let k = file.read("%s")\n    print(k)' % key, uses=("file",)))
    ro = run(s5, prog('    let k = file.read("other.txt")\n    print(k)', uses=("file",)))
    if "readable" not in sc(ro).get("output", ""):
        unmeasurable("MC-14k", "the control read of other.txt failed: " + brief(ro))
    elif sc(rk).get("verdict") == "blocked" and "PRIVATE KEY" not in sc(rk).get("output", ""):
        ok("MC-14k", "the attestation signing key is not readable by the attested program")
    else:
        bad("MC-14k", "a program read the attestation signing key", brief(rk))
    s5.close()
    s6 = Server(p_nobsd); s6.init2025()
    au = sc(run(s6, prog('    print("MC14U")'))).get("attestation") or {}
    if au.get("signed") is False and au.get("program_sha256"):
        ok("MC-14u", "with no signing key, attestations are produced and say signed=false")
    else:
        bad("MC-14u", "unsigned attestation wrong", json.dumps(au)[:300])
    s6.close()

    # ---- MC-01: 2026-07-28, stateless -----------------------------------------
    s7 = Server(p_nobsd)
    meta = {"io.modelcontextprotocol/protocolVersion": V2026,
            "io.modelcontextprotocol/clientCapabilities": {}}
    d = s7.request("server/discover", {"_meta": meta})
    dr = (d or {}).get("result", {})
    tl = s7.request("tools/list", {"_meta": meta})
    tr = (tl or {}).get("result", {})
    rr = s7.request("tools/call", {"_meta": meta, "name": "naab_run",
                                   "arguments": {"code": prog('    print("MC01")')}})
    bad_v = s7.request("tools/list", {"_meta": {"io.modelcontextprotocol/protocolVersion": "2099-01-01",
                                                "io.modelcontextprotocol/clientCapabilities": {}}})
    ver_err = (bad_v or {}).get("error", {})
    if (V2026 in dr.get("supportedVersions", []) and V2025 in dr.get("supportedVersions", [])
            and "tools" in dr.get("capabilities", {}) and dr.get("resultType") == "complete"
            and isinstance(dr.get("ttlMs"), int) and dr.get("cacheScope") in ("public", "private")
            and tr.get("resultType") == "complete" and TOOLS <= {t["name"] for t in tr.get("tools", [])}
            and (rr or {}).get("result", {}).get("resultType") == "complete"
            and "MC01" in sc(rr).get("output", "")
            and ver_err.get("code") == -32022 and "2099-01-01" == ver_err.get("data", {}).get("requested")):
        ok("MC-01", "2026-07-28 without initialize: discover, tools/list, tools/call carry resultType; unknown version is -32022")
    else:
        bad("MC-01", "2026-07-28 path wrong", brief(d) + " || " + brief(tl) + " || " + brief(rr) + " || " + brief(bad_v))
    s7.close()

    # ---- MC-15: protocol errors do not kill the server -----------------------
    s8 = Server(p_nobsd); s8.init2025()
    s8.send_raw("{this is not json")
    time.sleep(0.3)
    parse_err = [m for _, m in s8.notes if isinstance(m, dict) and m.get("error", {}).get("code") == -32700]
    parse_err += [m for m in s8.responses.values() if m.get("error", {}).get("code") == -32700]
    # An over-long line is refused and the reader resumes at the next line:
    # the bound on one message's memory must not cost the session.
    s8.send_raw('{"jsonrpc":"2.0","id":"big","method":"ping","params":{"pad":"' + "x" * (17 * 1024 * 1024) + '"}}')
    time.sleep(0.5)
    too_big = [m for m in s8.responses.values() if m.get("error", {}).get("code") == -32600
               and "too large" in m.get("error", {}).get("message", "")]
    um = s8.request("no/such_method", {})
    ut = s8.call("no_such_tool", {})
    ra = run(s8, prog('    print("MC15_ALIVE")'))
    if (parse_err and too_big and (um or {}).get("error", {}).get("code") == -32601
            and (ut or {}).get("error", {}).get("code") == -32602
            and "MC15_ALIVE" in sc(ra).get("output", "")):
        ok("MC-15", "malformed JSON -32700, a 17 MiB line -32600, unknown method -32601, unknown tool -32602; the server keeps serving")
    else:
        bad("MC-15", "protocol error handling wrong", "parse=%s too_big=%s um=%s ut=%s run=%s" % (bool(parse_err), bool(too_big), brief(um), brief(ut), brief(ra)))
    s8.close()

    # ---- MC-17: commits from one caller are serialised under BSD -------------
    hold_read = prog('    let k = env.get("MCP_TEST_SECRET") ?? "none"\n    time.sleep(2)\n    print("A")',
                     uses=("env", "time"))
    late_enc = prog('    time.sleep(1)\n    let e = crypto.base64_encode("abc")\n    print("B")',
                    uses=("time", "crypto"))
    s9 = Server(p_noscan, secret_env); s9.init2025()
    clear_outbox(p_noscan)
    ia = s9.start("tools/call", {"name": "naab_run", "arguments": {"code": hold_read}})
    time.sleep(0.3)
    ib = s9.start("tools/call", {"name": "naab_run", "arguments": {"code": late_enc}})
    ic = s9.start("tools/call", {"name": "naab_run", "arguments": {"code": send_c}})
    ra, rb, rc = s9.wait(ia, 60), s9.wait(ib, 60), s9.wait(ic, 60)
    serial_blocks = sc(rc).get("verdict") == "blocked" and not outbox_written(p_noscan)
    s9.close()
    # control: without BSD there is nothing to protect, and calls overlap
    s10 = Server(p_nobsd); s10.init2025()
    slow = prog('    time.sleep(2)\n    print("SLOW")', uses=("time",))
    i1 = s10.start("tools/call", {"name": "naab_run", "arguments": {"code": slow}})
    time.sleep(0.2)
    i2 = s10.start("tools/call", {"name": "naab_run", "arguments": {"code": prog('    print("FAST")')}})
    r1, r2 = s10.wait(i1, 60), s10.wait(i2, 60)
    overlapped = r1 is not None and r2 is not None and r2["__seq__"] < r1["__seq__"]
    s10.close()
    if not overlapped:
        bad("MC-17", "control: calls did not overlap with BSD disabled -- the server serialises everything",
            brief(r1) + " || " + brief(r2))
    elif serial_blocks:
        ok("MC-17", "a sequence split across concurrent calls is still blocked; without BSD calls overlap (control)")
    else:
        bad("MC-17", "concurrent calls split the sequence past the ledger", brief(ra) + " || " + brief(rb) + " || " + brief(rc))

    # ---- MC-18: output is scanned under no_secrets ---------------------------
    leak = prog('    let k = "AKIA" + "IOSFODNN7EXAMPLE"\n    print(k)')
    s11 = Server(p_main); s11.init2025()
    r18 = run(s11, leak)
    s11.close()
    s12 = Server(p_noscan); s12.init2025()
    r18c = run(s12, leak)
    s12.close()
    if AWS_EXAMPLE not in sc(r18c).get("output", ""):
        unmeasurable("MC-18", "control: the program did not print the value without the scan: " + brief(r18c))
    elif sc(r18).get("verdict") == "blocked" and AWS_EXAMPLE not in json.dumps(r18):
        ok("MC-18", "a secret-shaped value built at runtime is withheld under no_secrets and returned without it")
    else:
        bad("MC-18", "output reached the caller unscanned", brief(r18))

    # ---- MC-19: events streamed before a crash survive it --------------------
    if python_ok and send_works:
        s13 = Server(p_noscan, secret_env); s13.init2025()
        crash_after_read = prog('    let k = env.get("MCP_TEST_SECRET") ?? "none"\n'
                                '    let c = <<python\nimport ctypes\nctypes.string_at(0)\n>>\n    print(c)',
                                uses=("env",))
        rx = run(s13, crash_after_read)
        rcx = run(s13, enc_c)
        clear_outbox(p_noscan)
        rsx = run(s13, send_c)
        if sc(rx).get("verdict") in ("crashed", "error") and sc(rsx).get("verdict") == "blocked" and not outbox_written(p_noscan):
            ok("MC-19", "a read recorded before a crash still completes the sequence in a later call")
        else:
            bad("MC-19", "events were lost with the crashed worker", brief(rx) + " || " + brief(rsx))
        s13.close()
    else:
        unmeasurable("MC-19", "needs embedded python (MC-08) and a working send (MC-10 control)")

    # ---- report-only: propose latency ---------------------------------------
    s14 = Server(p_nobsd); s14.init2025()
    lat = []
    for _ in range(15):
        t = time.perf_counter()
        s14.call("naab_propose", {"code": prog('    print("x")')})
        lat.append((time.perf_counter() - t) * 1000)
    s14.close()
    lat.sort()
    print("INFO propose latency (observed, 15 serial calls): p50=%.1f ms p90=%.1f ms" %
          (lat[len(lat) // 2], lat[int(len(lat) * 0.9)]), flush=True)

    # ---- report-only: server memory across many calls ----------------------
    s15 = Server(p_nobsd); s15.init2025()

    def rss_kb():
        try:
            with open("/proc/%d/status" % s15.p.pid) as f:
                for ln in f:
                    if ln.startswith("VmRSS:"):
                        return int(ln.split()[1])
        except Exception:
            return -1
        return -1
    tiny = prog('    print("m")')
    for i in range(200):
        run(s15, tiny)
        if i == 9:
            rss10 = rss_kb()
    rss200 = rss_kb()
    s15.close()
    print("INFO server RSS (observed): %d KiB after 10 calls, %d KiB after 200" % (rss10, rss200), flush=True)

    return finish()


def finish():
    fails = sum(1 for s, _, _ in RESULTS if s == "FAIL")
    passes = sum(1 for s, _, _ in RESULTS if s == "PASS")
    skips = sum(1 for s, _, _ in RESULTS if s == "SKIP")
    print("ARMS: %d pass, %d fail, %d unmeasurable" % (passes, fails, skips), flush=True)
    return min(fails, 100)


if __name__ == "__main__":
    sys.exit(main())
