#!/usr/bin/env python3
"""Protection map: for each polyglot language x dangerous action x sandbox
level, WHICH layer stops the action -- the runtime (sandbox / executor), the
text checks, or nothing.

Why. The text checks (shell_injection, code_injection, banned_functions, ...)
read source text, and any text check can be evaded by building the call at
runtime. A cell where the runtime lets the action through and only a text
check stands in the way is where an evasion is a real escape; a cell the
runtime contains is one where a text-check miss costs nothing. Fixing the
stripper one bug at a time does not say which of those a bug sits in.

How. Every verdict is an OBSERVATION of the action, never of an exit code:
  exec   a child process creates a marker file          (file exists?)
  read   a file under capabilities.filesystem.blocked_paths is read
                                                          (its token printed?)
  write  a file under blocked_paths is written           (file exists?)
  net    a TCP connection reaches a local listener        (listener accepted?)
  env    an env var in env_vars.blocked_read is read      (its token printed?)
The secret tokens are random per probe and never appear in the source, so a
token in the output can only have come from performing the action.

Each probe runs the same block under:
  control  mode audit, sandbox unrestricted -- the action MUST happen here,
           or the snippet does not perform it and no verdict is drawn
           (UNMEASURABLE; NO_API where the runtime has no such API by design)
  runtime  mode audit at the level -- audit turns every text check into a log
           line but keeps the sandbox, so this measures containment alone
  text     mode enforce at the level -- runtime + text checks together
Cell verdict: CONTAINED (runtime stops it), TEXT-ONLY (runtime lets it
through, a text check blocks the plain form), OPEN (nothing stops it),
NO_API, UNMEASURABLE.

Usage: protmap.py [--naab build/naab-lang] [--json OUT] [--md OUT]
                  [--langs a,b] [--levels a,b] [--jobs N]
                  [--baseline FILE | --write-baseline FILE] [--cells SAVED.json]
       protmap.py --list-runtimes  (runtimes the binary registers)
"""

import argparse
import concurrent.futures
import json
import os
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ACTIONS = ["exec", "read", "write", "net", "env"]
# A column = sandbox level + policy profile. Every column's policy FORBIDS
# what it measures: vault/ is in blocked_paths, NAAB_PM_SECRET in
# env_vars.blocked_read, network.enabled is false. `deny` also sets
# shell.enabled false. `shell` leaves shell enabled -- the subprocess
# languages need it to run at all -- so exec is PERMITTED there, not probed.
COLUMNS = {
    "restricted/deny": ("restricted", False),
    "standard/deny": ("standard", False),
    "elevated/deny": ("elevated", False),
    "unrestricted/deny": ("unrestricted", False),
    "standard/shell": ("standard", True),
    "elevated/shell": ("elevated", True),
    "unrestricted/shell": ("unrestricted", True),
}
LEVELS = list(COLUMNS)
ENV_NAME = "NAAB_PM_SECRET"

# Each snippet performs ONE action with the language's ordinary API -- the
# form a text check is written for. {MARK} is the exec marker's file name,
# {PORT} the listener's port. Paths are relative to the probe's cwd.
SNIPPETS = {
    "python": {
        "alive": '"ALIVE"',
        "exec": 'import subprocess\nsubprocess.run(["touch", "{MARK}"])\n"done"',
        "read": 'open("vault/secret.txt").read()',
        "write": 'open("vault/w.txt", "w").write("x")\n"done"',
        "net": 'import socket\nsocket.create_connection(("127.0.0.1", {PORT}), timeout=3).close()\n"done"',
        "env": 'import os\nos.environ.get("NAAB_PM_SECRET", "none")',
    },
    "node": {
        "alive": 'console.log("ALIVE")',
        "exec": 'require("child_process").execFileSync("touch", ["{MARK}"]); console.log("done")',
        "read": 'console.log(require("fs").readFileSync("vault/secret.txt", "utf8"))',
        "write": 'require("fs").writeFileSync("vault/w.txt", "x"); console.log("done")',
        "net": 'const s = require("net").connect({PORT}, "127.0.0.1", () => { s.end(); console.log("done"); });',
        "env": 'console.log(process.env.NAAB_PM_SECRET || "none")',
    },
    "shell": {
        "alive": 'echo ALIVE',
        "exec": 'touch {MARK}',
        "read": 'cat vault/secret.txt',
        "write": 'echo x > vault/w.txt',
        "net": 'curl -s -m 3 http://127.0.0.1:{PORT}/ >/dev/null; echo done',
        "env": 'echo "$NAAB_PM_SECRET"',
    },
    "ruby": {
        "alive": 'puts "ALIVE"',
        "exec": 'system("touch", "{MARK}")',
        "read": 'puts File.read("vault/secret.txt")',
        "write": 'File.write("vault/w.txt", "x")',
        "net": 'require "socket"\nTCPSocket.new("127.0.0.1", {PORT}).close',
        "env": 'puts ENV["NAAB_PM_SECRET"]',
    },
    "php": {
        "alive": 'echo "ALIVE";',
        "exec": 'exec("touch {MARK}");',
        "read": 'echo file_get_contents("vault/secret.txt");',
        "write": 'file_put_contents("vault/w.txt", "x");',
        "net": '$s = fsockopen("127.0.0.1", {PORT}); fclose($s);',
        "env": 'echo getenv("NAAB_PM_SECRET");',
    },
    "go": {
        "alive": 'package main\nimport "fmt"\nfunc main() { fmt.Print("ALIVE") }',
        "exec": 'package main\nimport "os/exec"\nfunc main() { exec.Command("touch", "{MARK}").Run() }',
        "read": 'package main\nimport ("fmt"; "os")\nfunc main() { b, _ := os.ReadFile("vault/secret.txt"); fmt.Print(string(b)) }',
        "write": 'package main\nimport "os"\nfunc main() { os.WriteFile("vault/w.txt", []byte("x"), 0644) }',
        "net": 'package main\nimport "net"\nfunc main() { c, err := net.Dial("tcp", "127.0.0.1:{PORT}"); if err == nil { c.Close() } }',
        "env": 'package main\nimport ("fmt"; "os")\nfunc main() { fmt.Print(os.Getenv("NAAB_PM_SECRET")) }',
    },
    "cpp": {
        "alive": '#include <iostream>\nint main() { std::cout << "ALIVE"; return 0; }',
        "exec": '#include <cstdlib>\nint main() { return std::system("touch {MARK}") ? 1 : 0; }',
        "read": '#include <fstream>\n#include <iostream>\nint main() { std::ifstream f("vault/secret.txt"); std::cout << f.rdbuf(); return 0; }',
        "write": '#include <fstream>\nint main() { std::ofstream f("vault/w.txt"); f << "x"; return 0; }',
        "net": ('#include <arpa/inet.h>\n#include <sys/socket.h>\n#include <unistd.h>\n'
                'int main() { int s = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET;'
                ' a.sin_port = htons({PORT}); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);'
                ' connect(s, (sockaddr*)&a, sizeof a); close(s); return 0; }'),
        "env": '#include <cstdlib>\n#include <iostream>\nint main() { const char* v = std::getenv("NAAB_PM_SECRET"); std::cout << (v ? v : "none"); return 0; }',
    },
    "rust": {
        "alive": 'fn main() { print!("ALIVE"); }',
        "exec": 'fn main() { let _ = std::process::Command::new("touch").arg("{MARK}").status(); }',
        "read": 'fn main() { print!("{}", std::fs::read_to_string("vault/secret.txt").unwrap_or_default()); }',
        "write": 'fn main() { let _ = std::fs::write("vault/w.txt", "x"); }',
        "net": 'fn main() { let _ = std::net::TcpStream::connect("127.0.0.1:{PORT}"); }',
        "env": 'fn main() { print!("{}", std::env::var("NAAB_PM_SECRET").unwrap_or_default()); }',
    },
    # In-process QuickJS: no std/os modules are loaded (js_std_add_helpers adds
    # print/console only). The snippets try the QuickJS module APIs anyway, so
    # NO_API is still an observation -- the control must fail for it.
    "javascript": {
        "alive": '"ALIVE"',
        "exec": 'os.exec(["touch", "{MARK}"])',
        "read": 'std.loadFile("vault/secret.txt")',
        "write": 'let f = std.open("vault/w.txt", "w"); f.puts("x"); f.close(); "done"',
        "net": 'fetch("http://127.0.0.1:{PORT}/")',
        "env": 'std.getenv("NAAB_PM_SECRET")',
    },
    # In-process SQLite: an authorizer refuses ATTACH, so VACUUM INTO (which
    # attaches its target) is the only file write SQL has. No exec/net/env API.
    "sql": {
        "alive": "SELECT 'ALIVE' AS v;",
        "write": "VACUUM INTO 'vault/w.txt';",
        "read": "ATTACH DATABASE 'vault/secret.txt' AS s; SELECT * FROM s.sqlite_master;",
    },
}

# Runtimes that have NO such API by construction. A failed control there is
# reported NO_API; anywhere else a failed control is UNMEASURABLE (the snippet
# did not do what it claims, which proves nothing about containment).
NO_API = {
    "javascript": {"exec", "read", "write", "net", "env"},
    # read/write: ATTACH and VACUUM INTO are refused by the authorizer at
    # every level -- contained by construction, which the control proves.
    "sql": {"exec", "read", "write", "net", "env"},
}


def text_rules():
    """`restrictions` + `languages.per_language` from the langconform config,
    which turns every text check on. Only those two sections: the rest of
    that config (agents, hooks, telemetry) would change what a probe runs."""
    with open(os.path.join(REPO, "tools", "langconform", "config.json"), encoding="utf-8") as f:
        c = json.load(f)
    return c["restrictions"], c["languages"].get("per_language", {})


def make_config(mode, level, restrictions, per_language, policy=True):
    """policy=False is the CONTROL config: no path/env policy and no text or
    import rules at all, so nothing but the action itself decides."""
    if not policy:
        return {"version": "4.0", "mode": mode,
                "security": {"sandbox_level": level},
                "languages": {"allowed": [], "blocked": []}}
    sandbox, shell = COLUMNS[level]
    cfg = {
        "version": "4.0",
        "mode": mode,
        "security": {"sandbox_level": sandbox},
        "languages": {"allowed": [], "blocked": [], "per_language": per_language},
        "capabilities": {
            "shell": {"enabled": shell},
            "network": {"enabled": False},
            "filesystem": {"mode": "write", "blocked_paths": ["vault/"]},
            "env_vars": {"read": True, "blocked_read": [ENV_NAME]},
        },
        "restrictions": restrictions,
    }
    return cfg


class Listener:
    """One TCP listener per probe; `hits` counts accepted connections."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.hits = 0
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        while not self._stop:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            self.hits += 1
            try:
                c.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            c.close()

    def close(self):
        # A connection can still be in the backlog when the program exits
        # (Node connects asynchronously), so drain it before deciding: a
        # missed accept would read as "contained".
        self._stop = True
        self._t.join()
        self.sock.settimeout(1.0)
        while True:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                break
            self.hits += 1
            c.close()
        self.sock.close()


def run_probe(naab, lang, action, mode, level, rules, policy=True, timeout=90):
    """Run one block; return (happened, exit_code, output, audit_rules)."""
    w = tempfile.mkdtemp(prefix="protmap-")
    lis = Listener() if action == "net" else None
    try:
        os.makedirs(os.path.join(w, "vault"))
        file_token = "PMFILE" + secrets.token_hex(8)
        env_token = "PMENV" + secrets.token_hex(8)
        with open(os.path.join(w, "vault", "secret.txt"), "w") as f:
            f.write(file_token + "\n")
        mark = "pm_mark_" + secrets.token_hex(4)
        code = (SNIPPETS[lang][action]
                .replace("{MARK}", mark)
                .replace("{PORT}", str(lis.port if lis else 0)))
        with open(os.path.join(w, "govern.json"), "w") as f:
            json.dump(make_config(mode, level, *rules, policy=policy), f)
        with open(os.path.join(w, "p.naab"), "w") as f:
            # net: a runtime with an event loop (Node) returns from the block
            # with its connect still pending, and the program then exits and
            # kills it -- a race that read as "contained" in about 1 run in 3.
            # The pause keeps the process alive; nothing else changes.
            pause = "    time.sleep(1.5)\n" if action == "net" else ""
            use = "use time\n" if pause else ""
            f.write("%smain {\n    let r = <<%s\n%s\n>>\n%s    print(\"V=\" + string(r))\n}\n"
                    % (use, lang, code, pause))
        env = dict(os.environ)
        env[ENV_NAME] = env_token
        try:
            p = subprocess.run([naab, "p.naab", "--timeout", "60"], cwd=w, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
            rc = p.returncode
        except subprocess.TimeoutExpired:
            out, err, rc = "", "TIMEOUT", -1
        if action == "exec":
            happened = os.path.exists(os.path.join(w, mark))
        elif action == "read":
            happened = file_token in out or file_token in err
        elif action == "write":
            happened = os.path.exists(os.path.join(w, "vault", "w.txt"))
        elif action == "net":
            happened = lis.hits > 0
        elif action == "env":
            happened = env_token in out or env_token in err
        elif action == "alive":
            happened = "ALIVE" in out
        else:
            raise ValueError(action)
        audit = sorted({ln.split()[2].rstrip(":") for ln in err.splitlines()
                        if ln.startswith("[governance] AUDIT ")})
        return happened, rc, ("OUT:" + out[-400:] + " ERR:" + err[-800:]), audit
    finally:
        if lis:
            lis.close()
        shutil.rmtree(w, ignore_errors=True)


def static_block_rules(gov, lang, action, level, rules):
    """Rules naab-gov (static analysis only) blocks this snippet on, under the
    column's enforce config; None when naab-gov is unavailable."""
    if not os.path.exists(gov):
        return None
    code = SNIPPETS[lang][action].replace("{MARK}", "pm_mark").replace("{PORT}", "1")
    with tempfile.TemporaryDirectory(prefix="protmap-gov-") as w:
        cfg = os.path.join(w, "govern.json")
        with open(cfg, "w") as f:
            json.dump(make_config("enforce", level, *rules), f)
        p = subprocess.run([gov, "check", "--language", lang, "--config", cfg],
                           input=(code + "\n").encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=120)
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None
    return sorted({v["rule"] for v in doc.get("violations", [])
                   if not v["rule"].startswith("contradiction.")
                   and ("[HARD" in v.get("message", "") or "[SOFT" in v.get("message", ""))})


def registered_runtimes(naab):
    """Runtime names the binary has executors for (codegen.supported_languages,
    folded to runtimes with naab-gov's table); None when it cannot be asked."""
    gov = os.path.join(os.path.dirname(naab), "naab-gov")
    with tempfile.TemporaryDirectory(prefix="protmap-langs-") as w:
        with open(os.path.join(w, "govern.json"), "w") as f:
            json.dump({"version": "4.0", "mode": "audit",
                       "security": {"sandbox_level": "unrestricted"}}, f)
        with open(os.path.join(w, "l.naab"), "w") as f:
            f.write("use codegen\nmain {\n    let langs = codegen.supported_languages()\n"
                    "    for l in langs { print(l) }\n}\n")
        p = subprocess.run([naab, "l.naab"], cwd=w, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
    names = {ln.strip() for ln in p.stdout.decode("utf-8", "replace").splitlines()
             if ln.strip().isalpha()}
    if len(names) < 2 or not os.path.exists(gov):
        return None
    t = subprocess.run([gov, "languages"], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, timeout=60)
    try:
        table = json.loads(t.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None
    rows = table["languages"] if isinstance(table, dict) else table
    out = set()
    for n in names:
        rt = n
        for r in rows:
            if n == r.get("canonical") or n in r.get("aliases", []):
                rt = n if n in r.get("runtime_variants", []) else r["canonical"]
        out.add(rt)
    return sorted(out)


RANK = {"CONTAINED": 2, "NO_API": 2, "TEXT-ONLY": 1, "OPEN": 0}


def compare(cells, baseline):
    """(failures, skipped): a measured cell that differs from the baseline in
    EITHER direction fails -- weaker is a regression, stronger means the
    committed map (and docs/protection-map.md) is out of date."""
    fails, skipped = [], 0
    for key, want in sorted(baseline.items()):
        got = cells.get(key, {}).get("verdict")
        if got in (None, "UNMEASURABLE"):
            skipped += 1
            continue
        if got != want:
            d = ("WEAKER" if RANK.get(got, -1) < RANK.get(want, -1) else
                 "STRONGER" if RANK.get(got, -1) > RANK.get(want, -1) else "CHANGED")
            fails.append(f"{key}: {want} -> {got} ({d})")
    for key, c in sorted(cells.items()):
        if key not in baseline and c["verdict"] in RANK:
            fails.append(f"{key}: not in baseline, measured {c['verdict']}")
    return fails, skipped


def measure(naab, langs, levels, jobs):
    rules = text_rules()
    jobs_list = []
    for lang in langs:
        jobs_list.append((lang, "alive", "audit", "unrestricted", False))
        for lv in levels:  # does the language run at all in this column?
            jobs_list.append((lang, "alive", "audit", lv, True))
        for action in ACTIONS:
            if action not in SNIPPETS[lang]:
                continue
            jobs_list.append((lang, action, "audit", "unrestricted", False))  # control
            for lv in levels:
                if action == "exec" and COLUMNS[lv][1]:
                    continue  # permitted by this column's policy
                jobs_list.append((lang, action, "audit", lv, True))
                jobs_list.append((lang, action, "enforce", lv, True))
    results = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(run_probe, naab, *j[:4], rules, j[4]): j for j in jobs_list}
        for f in concurrent.futures.as_completed(futs):
            results[futs[f]] = f.result()

    cells = {}
    gov = os.path.join(os.path.dirname(naab), "naab-gov")
    for lang in langs:
        alive = results[(lang, "alive", "audit", "unrestricted", False)][0]
        for action in ACTIONS:
            for lv in levels:
                key = f"{lang}/{action}/{lv}"
                if action not in SNIPPETS[lang]:
                    cells[key] = {"verdict": "NO_API" if action in NO_API.get(lang, ()) else "NO_PROBE"}
                    continue
                if action == "exec" and COLUMNS[lv][1]:
                    cells[key] = {"verdict": "PERMITTED"}
                    continue
                ctl = results[(lang, action, "audit", "unrestricted", False)]
                rt = results[(lang, action, "audit", lv, True)]
                tx = results[(lang, action, "enforce", lv, True)]
                cell = {"control": ctl[0], "runtime_stops": not rt[0],
                        "text_stops": not tx[0], "text_exit": tx[1],
                        "text_rules": rt[3]}
                if not alive:
                    v = "UNMEASURABLE"
                    cell["why"] = "language did not run (toolchain absent?)"
                elif not ctl[0]:
                    if action in NO_API.get(lang, ()):
                        v = "NO_API"
                    else:
                        v = "UNMEASURABLE"
                        cell["why"] = "control: the action did not happen even unrestricted"
                        cell["control_output"] = ctl[2]
                elif not rt[0]:
                    v = "CONTAINED"
                    # display detail only (not baselined): the whole language
                    # is refused here, rather than this one action
                    cell["language_refused"] = not results[(lang, "alive", "audit", lv, True)][0]
                elif not tx[0]:
                    # Something in enforce mode stopped it. It is a TEXT check
                    # only if naab-gov -- which reads the source and never runs
                    # it -- blocks the same code under the same config. Audit
                    # mode also silences runtime gates routed through
                    # enforce(); one of those would show up here instead.
                    static = static_block_rules(gov, lang, action, lv, rules)
                    cell["static_rules"] = static
                    if static is None:
                        v = "UNMEASURABLE"
                        cell["why"] = "naab-gov not built: cannot attribute the block"
                    elif static:
                        v = "TEXT-ONLY"
                    else:
                        v = "CONTAINED"
                        cell["why"] = "runtime gate active only in enforce mode"
                else:
                    v = "OPEN"
                cell["verdict"] = v
                cells[key] = cell
    return cells


def render_md(cells, langs, levels):
    # runtime = the action is stopped while the language runs; refused = the
    # language does not run at all in this column (both are CONTAINED)
    abbrev = {"CONTAINED": "runtime", "TEXT-ONLY": "**TEXT**", "OPEN": "**OPEN**",
              "NO_API": "no-api", "UNMEASURABLE": "?", "NO_PROBE": "-",
              "PERMITTED": "(allowed)"}
    out = []
    for lv in levels:
        out.append(f"\n### {lv}\n")
        out.append("| language | " + " | ".join(ACTIONS) + " |")
        out.append("|---|" + "---|" * len(ACTIONS))
        for lang in langs:
            row = []
            for a in ACTIONS:
                c = cells[f"{lang}/{a}/{lv}"]
                row.append("refused" if c.get("language_refused") else abbrev[c["verdict"]])
            out.append(f"| {lang} | " + " | ".join(row) + " |")
    return "\n".join(out) + "\n"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--naab", default=os.path.join(REPO, "build", "naab-lang"))
    ap.add_argument("--langs", default=",".join(SNIPPETS))
    ap.add_argument("--levels", default=",".join(LEVELS))
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 2)))
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--baseline", help="compare against this baseline; exit 1 on a difference")
    ap.add_argument("--write-baseline", help="write the measured verdicts as a baseline")
    ap.add_argument("--list-runtimes", action="store_true")
    ap.add_argument("--cells", help="use this saved --json measurement instead of measuring")
    ap.add_argument("--profile", choices=["root", "nonroot"],
                    help="baseline section (default: from the effective uid)")
    a = ap.parse_args(argv)
    a.naab = os.path.abspath(a.naab)
    langs = [x for x in a.langs.split(",") if x]
    levels = [x for x in a.levels.split(",") if x]
    if a.list_runtimes:
        rts = registered_runtimes(a.naab)
        if rts is None:
            print("UNMEASURABLE: could not ask the binary for its languages")
            return 2
        print("\n".join(rts))
        return 0
    for l in langs:
        if l not in SNIPPETS:
            ap.error(f"no snippets for {l}")
    if a.cells:
        with open(a.cells, encoding="utf-8") as f:
            cells = json.load(f)
    else:
        cells = measure(a.naab, langs, levels, a.jobs)
    md = render_md(cells, langs, levels)
    sys.stdout.write(md)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(cells, f, indent=1, sort_keys=True)
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write(md)
    measured = {k: c["verdict"] for k, c in cells.items() if c["verdict"] in RANK}
    # The baseline has one section per privilege: RLIMIT_NPROC (the fork/exec
    # half of subprocess containment) is not enforced for root, so the same
    # build contains less when run as root -- measured: 9 cells differ.
    profile = a.profile or ("root" if hasattr(os, "geteuid") and os.geteuid() == 0 else "nonroot")
    if a.write_baseline:
        doc = {}
        if os.path.exists(a.write_baseline):
            with open(a.write_baseline, encoding="utf-8") as f:
                doc = json.load(f)
        doc[profile] = measured
        with open(a.write_baseline, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write("\n")
    if a.baseline:
        with open(a.baseline, encoding="utf-8") as f:
            doc = json.load(f)
        if profile not in doc:
            print(f"BASELINE no section for profile {profile!r} -- UNMEASURABLE")
            return 2
        base = doc[profile]
        print(f"PROFILE {profile}")
        fails, skipped = compare(cells, base)
        for line in fails:
            print("DIFF " + line)
        print(f"BASELINE {len(base) - skipped - len([x for x in fails if 'not in baseline' not in x])}"
              f" match, {len(fails)} differ, {skipped} unmeasurable here")
        return 1 if fails else 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
