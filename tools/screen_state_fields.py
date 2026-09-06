#!/usr/bin/env python3
"""A6 screening pass. For every DriftState/AgentTracker member: does anything
WRITE it, and can the writer be REACHED?

Register row A6 asks for a second screening pass with the A2 discipline, over
STATE FIELDS rather than config keys — A4 and A5 were unwritten/unrecordable
fields, a shape the key-oriented A2 sweep cannot see.

Run:  python3 tools/screen_state_fields.py
Exit 0 = self-test passed. A non-zero exit means the INSTRUMENT is broken; the
table it printed is not a result.

v1 produced nine "NEVER WRITTEN" and six were harness artifacts. Recorded here
because the failure modes are the point:

  1. subscript writes    state.cb_sustained_turns[i] = / signal_baselines[i].sum +=
  2. multi-line assigns  state.last_validation_shrank =\n    <expr>;
  3. missing accessor    it->second.trigger_penalty_mean =
  4. namesake risk       config_->thresholds.entity_context_window is NOT
                         DriftState::entity_context

So this version works on comment-stripped, semicolon-joined STATEMENTS, allows
`[...]` and `.sub` chains between the member and the operator, and carries a
SELF-TEST: members known to be written must come back written. A screening tool
whose own negatives are untested is the thing being warned about.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = ["src", "include"]

PERMISSIVE = [r"[A-Za-z_]\w*(?:\s*(?:\.|->)\s*\w+)*(?:\s*\[[^\];]*\])?"]
DRIFT_ACC = PERMISSIVE
TRACK_ACC = PERMISSIVE

# Instrument control: these MUST classify as written, or the tool is broken.
SELF_TEST_WRITTEN = {
    "DriftState": ["coherence_score", "cb_sustained_turns", "signal_baselines",
                   "last_validation_shrank", "trigger_penalty_mean",
                   "exercised_capabilities", "entity_context",
                   "plan_step_keywords", "instruction_history"],
    # AgentTracker controls matter more: it is declared in the file that uses it,
    # which is the shape that broke the declaration filter.
    "AgentTracker": ["nonce", "lease_expires_turn", "turns", "challenges_passed",
                     "last_challenge_turn", "key_offset"],
}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return "\n".join(l.split("//")[0] for l in text.split("\n"))


def statements(path):
    """(statement_text, first_line_no) with comments removed and lines joined."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    clean = strip_comments(raw)
    out, buf, start = [], [], 1
    for i, line in enumerate(clean.split("\n"), 1):
        if not buf:
            start = i
        buf.append(line)
        if ";" in line or "{" in line or "}" in line:
            out.append((" ".join(buf), start))
            buf = []
    if buf:
        out.append((" ".join(buf), start))
    return out


def source_files():
    for d in SRC_DIRS:
        for root, _, files in os.walk(os.path.join(REPO, d)):
            for f in files:
                if f.endswith((".cpp", ".h", ".hpp", ".cc")):
                    yield os.path.join(root, f)


def scan(members, accessors, declaring_file, decl_lo, decl_hi):
    acc = "|".join(accessors) if accessors is PERMISSIVE else "|".join(re.escape(a) for a in accessors)
    # accessor . member  then any chain of [..] or .sub / ->sub
    # Do NOT let the chain swallow a trailing `.method(` — that is what makes a
    # push_back/insert write read as a plain reference. v2's self-test caught it.
    # \b before the lookahead is load-bearing: without it the engine backtracks
    # `.insert` down to `.inser`, the lookahead then passes, and a mutating call
    # is consumed as a chain link. Second instrument bug the self-test caught.
    chain = r'(?:\s*\[[^\];]*\]|\s*(?:\.|->)\s*[a-z_]\w*\b(?!\s*\())*'
    result = {m: {"w": [], "r": []} for m in members}
    for path in source_files():
        rel = os.path.relpath(path, REPO)
        stmts = statements(path)
        # Pass 1: `auto& sightings = state.entity_context[entity];` — the member
        # is then written through `sightings`, which no direct grep for the
        # member name can see. This is the method doc's "indirection" check, and
        # it is why entity_context read as unwritten while push_back'd two lines
        # later. Map alias -> member, then count writes through the alias.
        alias = {}
        for stmt, _ln in stmts:
            am = re.search(rf'\bauto\s*&\s*([a-z_]\w*)\s*=\s*({acc})\s*(?:\.|->)\s*([a-z_]\w*)', stmt)
            if am and am.group(3) in result:
                alias[am.group(1)] = am.group(3)
        for aname, amem in alias.items():
            for stmt, line in stmts:
                for mo2 in re.finditer(rf'\b{re.escape(aname)}\b((?:\s*\[[^\];]*\]|\s*(?:\.|->)\s*[a-z_]\w*\b(?!\s*\())*)', stmt):
                    tail2 = stmt[mo2.end():]
                    if re.match(r'\s*(=[^=]|\+=|-=|\*=|/=|\+\+|--)', tail2) or re.match(
                        r'\s*(?:\.|->)?\s*(push_back|emplace_back|emplace|insert|clear|erase|assign|resize|pop_front|pop_back)\s*\(', tail2):
                        result[amem]["w"].append(f"{rel}:{line} (via &{aname})")
        for stmt, line in stmts:
            for m in members:
                pat = rf'\b({acc})\s*(?:\.|->)\s*{re.escape(m)}\b({chain})'
                for mo in re.finditer(pat, stmt):
                    tail = stmt[mo.end():]
                    is_write = bool(re.match(
                        r'\s*(=[^=]|=\s*$|\+=|-=|\*=|/=|%=|\|=|&=|\^=|<<=|>>=|\+\+|--)', tail
                    )) or bool(re.match(
                        r'\s*(?:\.|->)?\s*(push_back|emplace_back|emplace|insert|clear|erase|'
                        r'assign|resize|pop_front|pop_back|swap|reset)\s*\(', tail
                    ))
                    # A default-initialiser inside the struct BODY is a
                    # declaration, not a write. Scoping this to the file instead
                    # of the line range discarded every use of AgentTracker,
                    # which is declared in the same .cpp that uses it — five
                    # members came back "declared, never touched". Third
                    # instrument bug, and every one of them invented findings.
                    if rel == declaring_file and decl_lo < line < decl_hi:
                        continue
                    (result[m]["w"] if is_write else result[m]["r"]).append(f"{rel}:{line}")
    return result


def struct_members(rel_path, start, end):
    out = []
    for i, line in enumerate(open(os.path.join(REPO, rel_path), encoding="utf-8"), 1):
        if not (start < i < end):
            continue
        t = line.strip()
        if not t or t.startswith(("//", "*", "/*")):
            continue
        if "(" in t.split("=")[0]:
            continue
        mo = re.match(r'^[A-Za-z_][\w:<>,\s\*&]*?\b([a-z_][a-z0-9_]*)\s*(\[[^\]]*\])?\s*(=|;|\{)', t)
        if mo and mo.group(1) not in ("return", "if", "else", "struct", "const", "static", "using"):
            out.append(mo.group(1))
    return sorted(set(out))


def main():
    targets = [
        ("DriftState", "include/naab/behavioral_sequence.h", 158, 484, DRIFT_ACC),
        ("AgentTracker", "src/stdlib/agent_impl.cpp", 77, 102, TRACK_ACC),
    ]
    failures = 0
    for name, rel, a, b, acc in targets:
        members = struct_members(rel, a, b)
        res = scan(members, acc, rel, a, b)

        # --- instrument control, before any result is read ---
        for m in SELF_TEST_WRITTEN.get(name, []):
            if m in res and not res[m]["w"]:
                print(f"!! SELF-TEST FAIL: {name}.{m} known-written but classified unwritten")
                failures += 1
        print(f"\n{'='*104}\n{name}  ({rel}:{a}-{b}) — {len(members)} members\n{'='*104}")
        print(f"{'member':40s} {'W':>3s} {'R':>3s}  outcome")
        print("-"*104)
        rows = []
        for m in members:
            w, r = res[m]["w"], res[m]["r"]
            if not w and not r:
                o = "NO ACCESS — declared, never touched"
            elif not w:
                o = "NEVER WRITTEN — reads: " + ", ".join(sorted(set(r))[:3])
            elif not r:
                o = "WRITTEN, NEVER READ — writes: " + ", ".join(sorted(set(w))[:3])
            else:
                o = "ok"
            rows.append((m, len(set(w)), len(set(r)), o))
        rank = {"NO ACCESS": 0, "NEVER WRI": 1, "WRITTEN, ": 2, "ok": 3}
        rows.sort(key=lambda x: (rank.get(x[3][:9], 9), x[0]))
        for m, nw, nr, o in rows:
            if o != "ok":
                print(f"{m:40s} {nw:3d} {nr:3d}  {o}")
        n_sus = sum(1 for x in rows if x[3] != "ok")
        print(f"\n  {n_sus} of {len(rows)} members flagged; {len(rows)-n_sus} written-and-read.")
    print(f"\nself-test failures: {failures}")
    return 1 if failures else 0


sys.exit(main())
