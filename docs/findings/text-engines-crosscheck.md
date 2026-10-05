# Findings: the two text engines, on the same inputs

Measured 2026-10-05 with `tools/langconform/crosscheck.py` (report only).
Re-run: `python3 tools/langconform/crosscheck.py --gov build/naab-gov`.

NAAb judges source text with two separate regex engines:

| | Polyglot checker | C++ scanner (naab-q) |
|---|---|---|
| Code | `checkPolyglotBlock()`, `src/runtime/governance_checks.cpp` | `src/scanner/` |
| Reached by | every polyglot block at runtime (enforcing); `naab-gov check` | `naab-gov scan`; advisory preflight over `.naab` files |
| Text model | strings stripped per language; comments kept for most rules | raw lines; no string or comment model |

NAAb code itself is also judged by a third, parse-tree engine (taint, contracts,
function capabilities). It is not compared here; nothing on the text side
can do what it does.

## Method

- **Attribution without a rule-name map.** Every dangerous snippet has a benign
  twin of the same shape (`eval(user_input)` / `len(user_input)`). A rule counts
  only if it fires on the snippet and not on its twin.
- **Positions.** Each snippet is placed as code, inside the language's own line
  comment, and inside a plain string.
- **Configs differ, and that is a stated confound.** The scanner runs with its
  defaults (no govern.json); the checker runs with the template in audit mode
  (`tools/langconform/config.json`). The report says which engine fired; it
  does not claim why.

## Results (observed)

Where the construct is real (as code, or in a string for concepts that live
in strings), over 30 concept/language cells:

| Outcome | Cells |
|---|---|
| Both engines fire | 17 |
| Checker only | 6 |
| Scanner only | 1 |
| Neither | 6 |

- **Shell execution** (`os.system`, `child_process.exec`, `exec.Command`,
  `Command::new`, `system()`): the checker catches all 5 languages, the scanner
  none.
- **Python/JS `eval`**: the scanner reports it as `insecure_deserialization`.
- **Neither engine flags:**
  - JS `md5`;
  - JS `Math.random()`;
  - path traversal written inside a string, the normal way. The checker's path
    check reads string-stripped text by design ("FIX 16"), so it can only see
    an unquoted path. In practice that means a comment, which is where it fires
    here.
- **Insecure randomness**: the scanner only (Python); the checker never.
- **Comments**: the scanner never fires inside a comment; the checker fires on
  dangerous calls inside comments in 14 cells. Most checker rules read text
  where comments are kept (they exist for placeholder/temporary-code rules).
  That errs strict, not a bypass.
- **Strings**: the scanner fires on dangerous text inside plain strings; the
  checker mostly does not (only `dangerous_calls`, which also reads raw text).

## What this means for merging them

The engines overlap in concept and disagree in detail. Merging is a matter of
one text model (code / comments / strings, from the language table) and one
rule catalogue that both entry points run. Every rule's findings will move,
in both directions, so each step needs the same before/after diff as
`tools/langconform` gives the checker.
