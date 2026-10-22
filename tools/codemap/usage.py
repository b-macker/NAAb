"""usage: how sessions use the code map, read from Claude Code transcripts.

Roadmap item 10. Every Bash tool call in the transcripts that runs
tools/codemap/codemap.py is counted, per subcommand:

  calls       code-map invocations (a command with two invocations counts two)
  errors      the tool result was an error, or printed a traceback or a
              `codemap: no ...` refusal (screened: attributed to every
              invocation in that tool call)
  repeats     the next code-map invocation was the same command
  grep-after  one of the next 3 tool calls greps for the invocation's first
              argument (screened: the grep may have had another reason)

It prints subcommand names and counts only -- never command text, arguments
or output -- so a report can be shared without carrying what the session read.

Limit, printed with every report: transcripts written by the session that
built a tool are AUTHORED usage, not observed. Nothing is removed on this
evidence; it is a lead for later.
"""

import json
import re
import shlex
import sys

# An invocation is python running codemap.py: the bare word also appears in commit
# messages, echoed text and grep patterns, which the first version counted.
INVOKE = re.compile(r"\bpython3?\s+(?:[\w.\"'$/{}-]*/)?codemap\.py\"?")
HERE = __import__("os").path.dirname(__import__("os").path.abspath(__file__))


def known_subcommands():
    """The subcommands codemap.py's own parser lists in its --help -- a typed list is
    right only the day it is typed, and a regex over its source missed the two it
    adds in a loop (anchors, jump)."""
    import subprocess
    r = subprocess.run([sys.executable, __import__("os").path.join(HERE, "codemap.py"), "-h"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    m = re.search(r"\{([a-z,-]+)\}", r.stdout.decode("ascii", "replace"))
    return set(m.group(1).split(",")) if m else set()
SEPARATORS = re.compile(r"&&|\|\||[;|\n]|\$\(")
VALUE_OPTIONS = ("--repo", "--cache")
GREP_WINDOW = 3


# A name bound to the invocation in the same command: C="python3 .../codemap.py --repo X"
# (then `$C callers`), or cm() { python3 .../codemap.py ...; } (then `cm callers`).
ALIAS_VAR = re.compile(r"\b([A-Za-z_]\w*)=[\"']?[^\n;&|]*?codemap\.py")
ALIAS_FN = re.compile(r"\b([A-Za-z_]\w*)\s*\(\)\s*\{[^}]*?codemap\.py")


def _starts(command):
    """Where invocations start: python3 .../codemap.py, and the names the same
    command bound to it, used at the start of a command segment."""
    starts = [m.end() for m in INVOKE.finditer(command)]
    names = set(ALIAS_VAR.findall(command)) | set(ALIAS_FN.findall(command))
    for n in names:
        use = re.compile(r"(?:^|[;&|\n(]|&&|\|\|)\s*(?:\$\{?%s\}?|%s)(?=\s)" % (re.escape(n), re.escape(n)))
        starts += [m.end() for m in use.finditer(command)]
    return sorted(set(starts))


def invocations(command):
    """(subcommand, first argument, normalised tokens) for each codemap run in a
    shell command. Options before the subcommand are skipped."""
    found = []
    for start in _starts(command):
        rest = command[start:]
        stop = SEPARATORS.search(rest)
        seg = rest[:stop.start()] if stop else rest
        try:
            toks = shlex.split(seg)
        except ValueError:
            toks = seg.split()
        i = 0
        while i < len(toks) and toks[i].startswith("-"):
            i += 2 if toks[i] in VALUE_OPTIONS else 1
        if i >= len(toks) or not re.fullmatch(r"[a-z][a-z-]*", toks[i]):
            continue
        sub = toks[i]
        arg = next((t for t in toks[i + 1:] if not t.startswith("-")), "")
        found.append((sub, arg, tuple(toks[i:])))
    return found


def _text(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(b.get("text", "") for b in content if isinstance(b, dict))
    return ""


def read(paths):
    """The tool calls of every transcript, in order: dicts with the tool name,
    its input, and (filled from the result) whether it errored."""
    calls, by_id, bad = [], {}, 0
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                try:
                    d = json.loads(ln)
                except ValueError:
                    bad += 1
                    continue
                msg = d.get("message") if isinstance(d, dict) else None
                content = msg.get("content") if isinstance(msg, dict) else None
                if not isinstance(content, list):
                    continue
                for b in content:
                    if not isinstance(b, dict):
                        continue
                    if b.get("type") == "tool_use":
                        c = {"name": b.get("name", ""), "input": b.get("input") or {}, "error": False}
                        calls.append(c)
                        by_id[b.get("id")] = c
                    elif b.get("type") == "tool_result" and b.get("tool_use_id") in by_id:
                        text = _text(b.get("content"))
                        if b.get("is_error") or "Traceback (most recent call last)" in text \
                                or re.search(r"^codemap: (no |refusing)", text, re.M):
                            by_id[b["tool_use_id"]]["error"] = True
    return calls, bad


def _greps_for(call, name):
    if len(name) < 4:
        return False
    inp = call["input"]
    if call["name"] == "Grep":
        return name in str(inp.get("pattern", ""))
    if call["name"] == "Bash":
        cmd = str(inp.get("command", ""))
        return bool(re.search(r"\b(?:rg|grep)\b", cmd)) and name in cmd and not INVOKE.search(cmd)
    return False


def report(paths):
    calls, bad = read(paths)
    known = known_subcommands()
    stats, seq = {}, []
    for idx, c in enumerate(calls):
        if c["name"] != "Bash":
            continue
        for sub, arg, toks in invocations(str(c["input"].get("command", ""))):
            seq.append((idx, sub if sub in known else "(not a subcommand)", arg, toks, c["error"]))
    prev = None
    for idx, sub, arg, toks, err in seq:
        s = stats.setdefault(sub, {"calls": 0, "errors": 0, "repeats": 0, "grep_after": 0})
        s["calls"] += 1
        s["errors"] += 1 if err else 0
        if prev == toks:
            s["repeats"] += 1
        prev = toks
        if any(_greps_for(calls[j], arg) for j in range(idx + 1, min(idx + 1 + GREP_WINDOW, len(calls)))):
            s["grep_after"] += 1
    return {"transcripts": len(paths), "tool_calls": len(calls), "unreadable_lines": bad,
            "codemap_calls": len(seq), "subcommands": stats}


LIMIT = ("LIMIT: transcripts written by the session that built a tool are authored usage, not observed;"
         " nothing is removed on this evidence. grep-after and errors are screened; an invocation through a"
         " name bound in an EARLIER tool call, a script file, or a test suite is not seen.")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    as_json = "--json" in argv
    paths = [p for p in argv if p != "--json"]
    if not paths:
        sys.stderr.write("usage: usage.py [--json] TRANSCRIPT.jsonl...\n")
        return 2
    res = report(paths)
    res["limit"] = LIMIT
    if as_json:
        sys.stdout.buffer.write((json.dumps(res, indent=1, ensure_ascii=True) + "\n").encode("ascii"))
        return 0
    lines = ["codemap usage from %d transcript(s), %d tool calls (%d unreadable lines): %d code-map calls"
             % (res["transcripts"], res["tool_calls"], res["unreadable_lines"], res["codemap_calls"]),
             "  %-14s %6s %7s %8s %11s" % ("subcommand", "calls", "errors", "repeats", "grep-after")]
    for sub, s in sorted(res["subcommands"].items(), key=lambda kv: -kv[1]["calls"]):
        lines.append("  %-14s %6d %7d %8d %11d" % (sub, s["calls"], s["errors"], s["repeats"], s["grep_after"]))
    lines.append("  " + LIMIT)
    sys.stdout.buffer.write(("\n".join(lines) + "\n").encode("ascii", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
