#!/usr/bin/env python3
"""Which command-line options does src/cli/main.cpp parse, and has each one been
classified against govern.json's policy?

GOVERN.JSON IS PRIMARY (CLAUDE.md): "All settings belong in govern.json first.
CLI flags are overrides only. Never add behavior that requires a CLI flag
without a govern.json equivalent." A new option therefore has to have an
answer to one question before it lands: how does it relate to governance? Can
it waive or loosen enforcement (and is that defended), does it override a
govern.json setting, does it only observe governance, does it administer a
governance artifact, or does it have nothing to do with govern.json at all?
Nothing forced that answer to be written down, so this guard does.

EXTRACTION, SCREENED (like tests/cli/test_flag_position.sh's FP-S). This tool
reads main.cpp, strips C/C++ comments while respecting string and char
literals, and collects every exact string literal of the form "--name". That
is how the options are written at their parse sites:

    arg == "--governance-report"
    arg.rfind("--config=", 0) == 0          (prefix forms kept)
    checkBlocked("--no-governance", ...)    (the bypass-flag defense)
    findFlagValue("--authorized-by", ...)   (value read on the next line)

A "--name" literal that lives only inside a // or /* */ comment (main.cpp has
"--flag" in a comment explaining the blocked-flags sweep) is NOT an option and
must not be collected -- the comment strip is what keeps it out, and `self-test`
is the positive control that the strip works and that the extractor finds the
planted literal (an extractor that finds nothing cannot pass).

It only EXTRACTS and SELF-TESTS. The classification itself lives in
tests/cli/flag_policy.txt (one line per option: option<TAB>category<TAB>reason),
and tests/cli/test_flag_policy.sh compares that file against this extraction and
checks that a bypass-guarded line still names a live main.cpp function. This
tool decides nothing about policy; it only says which options exist.

Run:  python3 tools/flag_policy_coverage.py extract      # reads source on stdin
      python3 tools/flag_policy_coverage.py self-test     # embedded fixture
"""
import re
import sys

# A long option literal: "--" then at least one alnum, then alnum or '-'.
_OPT = re.compile(r'"(--[A-Za-z0-9][A-Za-z0-9-]*)"')


def strip_comments(src: str) -> str:
    """Remove // and /* */ comments, respecting string and char literals.

    Newlines inside removed comments are kept so line numbers do not shift for
    any caller that cares; the content is what matters here. A naive split on
    "//" would corrupt a literal such as "https://..." -- this walks the text
    as a tiny C++ tokenizer instead so only true comments go.
    """
    out = []
    i, n = 0, len(src)
    state = "code"  # code | string | char | line_comment | block_comment
    while i < n:
        c = src[i]
        d = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == '"':
                state = "string"; out.append(c); i += 1
            elif c == "'":
                state = "char"; out.append(c); i += 1
            elif c == "/" and d == "/":
                state = "line_comment"; i += 2
            elif c == "/" and d == "*":
                state = "block_comment"; i += 2
            else:
                out.append(c); i += 1
        elif state == "string":
            if c == "\\":
                out.append(c); out.append(d); i += 2
            elif c == '"':
                state = "code"; out.append(c); i += 1
            else:
                out.append(c); i += 1
        elif state == "char":
            if c == "\\":
                out.append(c); out.append(d); i += 2
            elif c == "'":
                state = "code"; out.append(c); i += 1
            else:
                out.append(c); i += 1
        elif state == "line_comment":
            if c == "\n":
                state = "code"; out.append(c); i += 1
            else:
                i += 1
        elif state == "block_comment":
            if c == "*" and d == "/":
                state = "code"; i += 2
            else:
                if c == "\n":
                    out.append(c)
                i += 1
    return "".join(out)


def extract(src: str):
    """Sorted, de-duplicated list of long options parsed in `src`."""
    return sorted(set(_OPT.findall(strip_comments(src))))


_FIXTURE = r'''
// A fixture mirroring main.cpp's shapes. The planted literal must be FOUND,
// the comment literals must NOT be.  "--comment-flag" here is a decoy.
int run(int argc, char** argv) {
    std::string arg = argv[1];
    if (arg == "--planted-flag") {            // real option, must be found
        return 0;
    } else if (arg == "--value-opt" && i + 1 < argc) {
        value = argv[++i];                    // "--trap-in-comment" decoy
    }
    const char* url = "https://example.invalid/--not-an-option";  // not "--"
    /* a block comment mentioning "--block-comment-flag" must be ignored */
    checkBlocked("--planted-guarded", used);
    return 0;
}
'''


def self_test() -> int:
    """Positive control: the extractor finds a planted literal and excludes
    every literal that lives only in a comment. Returns the failure count."""
    found = extract(_FIXTURE)
    failures = []
    if "--planted-flag" not in found:
        failures.append("planted option --planted-flag was not extracted")
    if "--value-opt" not in found:
        failures.append("planted option --value-opt was not extracted")
    if "--planted-guarded" not in found:
        failures.append("checkBlocked() option --planted-guarded was not extracted")
    for decoy in ("--comment-flag", "--trap-in-comment", "--block-comment-flag"):
        if decoy in found:
            failures.append("comment-only literal %s was extracted" % decoy)
    if "--not-an-option" in found:
        failures.append("a URL path segment was taken for an option")
    if not found:
        failures.append("extractor found nothing on the fixture")
    for f in failures:
        print("self-test FAIL: %s" % f)
    print("self-test failures: %d" % len(failures))
    print("self-test extracted: %s" % " ".join(found))
    return len(failures)


def main(argv) -> int:
    if len(argv) < 2 or argv[1] not in ("extract", "self-test"):
        sys.stderr.write(__doc__ or "")
        return 2
    if argv[1] == "self-test":
        return 1 if self_test() != 0 else 0
    # extract: source on stdin (keeps a native Windows python off MSYS paths).
    src = sys.stdin.buffer.read().decode("utf-8", "replace")
    for opt in extract(src):
        print(opt)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
