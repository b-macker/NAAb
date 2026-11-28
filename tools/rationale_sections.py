#!/usr/bin/env python3
"""The govern.json sections whose `rationale` the loader reads (A233).

Reads src/runtime/governance_config.cpp and prints one JSON path per line: the
object every `parseRationale(OBJ, ...)` call reads from, resolved back to the
top of the config through the assignments that name it (`auto& net =
cap["network"];`, `auto& cap = j["capabilities"];`), plus the one section that
reads its rationale directly (`cg["rationale"]`). An entry of a list or map
prints as `custom_rules[]`, `governance_plugins[].rules[]`,
`contracts.functions.*`; the simple code_quality checks loaded through
`loadSimpleCheck("KEY", ...)` print one path each.

tests/governance_v4/test_rationale_sections.sh compares this list with
rationale_sections.psv, so a section that starts reading a rationale needs a
row there -- and the row is then checked against the engine's rule-to-section
table (GovernanceEngine::ruleSectionInfo()), the table a refusal, a report and
naab-gov context read the rationale from.

Screened, not traced: it reads source text, so an object reached any other way
than these assignment shapes prints as `?UNRESOLVED <var> line N` -- never
dropped, so the comparison fails on it rather than passing without it.

stdlib only. Usage: rationale_sections.py [governance_config.cpp]
"""
import re
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "src/runtime/governance_config.cpp"
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read().split("\n")

    assign = re.compile(r'(?:const\s+)?(?:auto|nlohmann::json)\s*&?\s*(\w+)\s*=\s*(\w+)\[\s*"([a-z_]+)"\s*\]\s*;')
    loop = re.compile(r'for\s*\(\s*(?:const\s+)?auto\s*&?\s*(?:\[\s*\w+\s*,\s*)?(\w+)\s*\]?\s*:\s*(\w+)\[\s*"([a-z_]+)"\s*\](\.items\(\))?\s*\)')

    def resolve(var, line_no, depth=0):
        """The JSON path of variable `var` as seen from line `line_no`."""
        if depth > 12:
            return None
        if var == "j":
            return ""
        for k in range(line_no, max(-1, line_no - 1500), -1):
            m = assign.search(src[k])
            if m and m.group(1) == var:
                parent = resolve(m.group(2), k, depth + 1)
                if parent is None:
                    return None
                return (parent + "." if parent else "") + m.group(3)
            m = loop.search(src[k])
            if m and m.group(1) == var:
                parent = resolve(m.group(2), k, depth + 1)
                if parent is None:
                    return None
                base = (parent + "." if parent else "") + m.group(3)
                return base + (".*" if m.group(4) else "[]")
        return None

    out = []
    call = re.compile(r'parseRationale\(\s*(\w+)\s*,')
    simple = re.compile(r'loadSimpleCheck\(\s*"([a-z_]+)"')
    simple_keys = [m.group(1) for line in src for m in [simple.search(line)] if m]
    for i, line in enumerate(src):
        if "static void parseRationale" in line:
            continue
        m = call.search(line)
        if m:
            obj = m.group(1)
            if obj == "obj":   # loadSimpleCheck's body: one path per key it is called with
                cq = None
                for k in range(i, max(-1, i - 80), -1):
                    mm = re.search(r'auto\s*&\s*obj\s*=\s*(\w+)\[\s*key\s*\]', src[k])
                    if mm:
                        cq = resolve(mm.group(1), k)
                        break
                if cq is None:
                    out.append(f"?UNRESOLVED obj line {i + 1}")
                else:
                    out.extend(f"{cq}.{key}" for key in simple_keys)
                continue
            p = resolve(obj, i)
            out.append(p if p else f"?UNRESOLVED {obj} line {i + 1}")
        m = re.search(r'(\w+)\[\s*"rationale"\s*\]\.get<std::string>\(\)', line)
        # parseRationale()'s own body reads obj["rationale"]: that is the helper, not a section.
        if m and "parseRationale" not in line and "target = obj[" not in line:
            p = resolve(m.group(1), i)
            out.append(p if p else f"?UNRESOLVED {m.group(1)} line {i + 1}")
    for p in sorted(set(out)):
        print(p)


if __name__ == "__main__":
    main()
