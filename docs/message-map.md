# Model-visible message map

Every place NAAb puts text in front of a reader that acts on it -- a person, a
coding model reading the terminal, an agent model inside `agent.send()`, the
governed script, or an MCP caller -- traced backward from where the text lands
to where it is produced. The purpose is to know what already steers a model
toward what `govern.json` asks for, where those messages converge, and where
they are damaged, lost or unfiltered, BEFORE anything new (live guidance from
databases, MCP or HTTP sources) is added to them.

**Status (2026-10-15): three passes.** Pass 1 traced backward from the sinks
and probed the key paths. Pass 2 was independent of it: three enumerations of
the code made without sight of pass 1's map, diffed against it, every
difference that mattered verified by a probe on both engines; it changed five
pass-1 claims. Pass 3 ran every channel the first two had only traced (REST,
the REPL, `naab-gov`, the LSP, the report files, the package manager) with one
probe program, and found two defects in the loosening direction plus a prior
record pass 1 and 2 missed. Each pass changed something, so by the stopping
rule (`docs/investigation-method.md`) the map is still not finished. Tiers on every
claim: **verified** (a probe on the current build, both engines unless said),
**traced** (followed through the source), **screened** (grep or count only).
Probe configs and stub fixtures were authored for these passes (configured
runs); no number here comes from a live provider. Defects found are rows A38 to
A46 in `docs/open-investigations.md`.

## 1. Who reads

| Reader | What reaches it |
|---|---|
| A. Coding model or person at the terminal of `naab-lang run` | stdout and stderr |
| B. Agent model inside `agent.send()` | the request the engine assembles, and tool results |
| C. The governed script | the `agent.*` response dicts, `governance.*` functions, catchable errors |
| D. Operator after the run | reports, telemetry, transcript, the opt-in dashboard |
| E. MCP caller (`naab-lang mcp`) | `output` (worker stdout) and `findings` (worker stderr) |
| F. Editor via the LSP (`tools/naab-lsp`) | diagnostics parsed from `naab-lang --lint-only` output: code `governance`, line 0, first line only (A43) |
| G. REST client (`naab-lang api`) | `output` (the io capture stream) and `error` (sanitized exception text); governance stderr stays on the server; per-function checks never ran (A45, fixed 2026-10-19: they read the submitted text now) |
| H. `naab-gov lint` / `check` | lint: everything on stderr, unsanitized, guidance appended; check: JSON with `hard_block` (with guidance) and `violations[].message` (without) |
| I. REPL (`naab-lang` with no arguments) | no governance, by decision (`docs/GOVERNANCE-LIMITS.md` §5.2); errors as `Error:` on stderr |

## 2. Where it converges

1. **One governance message shape**: `GovernanceEngine::formatError()`
   (`governance_config.cpp:177`), 141 calls (screened): `Governance
   error|warning: <what> [TAG]`, `At:`, `Rule:`, `Help:`, `Example: ✗ Blocked /
   ✓ Allowed`, `Note:` for advisories.
2. **One verdict router**: `enforce()` (`governance_engine.cpp:1217-1654`), 186
   call sites (screened). It returns `""` or throws -- never the message
   (traced). HARD, SOFT without override, DETECT and APPROVAL denied throw; the
   text reaches the terminal through the catch sites below. ADVISORY prints to
   stderr itself: full detail on the FIRST occurrence of a rule name per run,
   `(occurrence n/m)` afterwards only when escalation is on (decided,
   `aacade58`). Live guidance is appended as a suffix (`shownWithGuidance`);
   hooks and the refusal attestation receive the message without it.
3. **The fatal catch sites**, `main.cpp:3011-3083`: every uncaught error --
   governance HARD, runtime, parse, polyglot -- is printed to **stdout** after
   `ErrorSanitizer::sanitize()`; each first calls `flushGroupedAdvisories()` --
   which finds no engine on the tree-walker (A43).
4. **The end-of-run sequence** (stderr), VM `vm.cpp:194-215`, tree-walker
   `interpreter.cpp:1061-1090`: `flushGroupedAdvisories()` (grouped `[ADVISORY]`
   blocks for duplicate calls, polyglot try/catch and undeclared function
   effects; the suppression count; the `-- Guidance:` block), then the voice,
   then `[governance] Summary`, then reports and hooks; `main.cpp` then prints
   the validation line `-- Governance: PASS (...)`, contract and quality-gate
   failures, baseline regression and the opt-in dashboard.
5. **Agent request assembly**, `agent_impl.cpp:2760-2870`: the last user
   message becomes `[reminder or correction]` + `[project guidance]` + the
   script's text (verified order by trace). Tool results return to the model
   as the value, a fixed `Error: ...` string, or the exception text with paths
   replaced and a 256-character cap (`agent_impl.cpp:3693-3705`).

## 3. Terminal output, in the order it appears

| Group | Stream | Shape (verified unless marked) |
|---|---|---|
| Load | stderr | `[governance] Warning: ...` one-liners (unknown key with a "did you mean"); CONTRA findings in the full advisory shape; `[governance] Loaded:` banner; integrity blocks |
| Static checks | stderr | `[governance] WARNING <rule>`, the `formatError` block, then `Project guidance (<source> > <section>):` |
| Pre-flight | stderr | type-check, lockfile, `[scanner]` pre-flight notes (VM, traced), agent review block and `[hint]` rejected findings |
| Runtime | stderr | the same advisory shape, `(occurrence n/m)`, `ESCALATED`; `[hint] dict.get("k") returned null -- did you mean "..."?` (both engines, capped at 3); DX notes |
| Polyglot | **stdout** | a block's own stderr as `[shell stderr]: ...` while its stdout becomes the block's value; `[PY ADAPTER ERROR]` before a Python error |
| Fatal | **stdout** | `Error: ...` in one of four shapes: governance (`formatError`), runtime (hand-written Got/Help/Example), parse (location plus source excerpt), polyglot (passed through, no help) |
| End of run | stderr | grouped advisories, guidance block, voice, Summary, `-- Governance:` line |

A pipe carrying both streams kept program lines and warnings in order (one
sample).

## 4. Message shapes

- Governance: the `formatError` shape above.
- Runtime and stdlib: hand-written per site. 313 string literals carry `Help:`
  and 194 carry `Example:` (screened); `agent_impl.cpp` 85, `polyglot.cpp` 31,
  `governance_engine.cpp` 24, `expressions.cpp` 22, `parser.cpp` and
  `call_dispatch.cpp` 18 each lead the `Help:` count. At least four example
  conventions: `✗ Wrong / ✓ Right` (UTF-8 and as byte escapes), ASCII `x Wrong
  / v Right` (the form CLAUDE.md prescribes), `✗ Blocked / ✓ Allowed`
  (governance), plus single-site variants (screened).
- Parse: `Parser::formatError` (`parser.cpp:504`), location, `Got:`, a source
  excerpt and hints.
- "Did you mean" exists three times: `utils::findSimilar` (12 stdlib modules),
  `error::suggestForUndefinedVariable` (4 sites), `GovernanceEngine::suggestKey`
  (config keys) (screened).
- Never linked into `naab-lang` (verified with `nm`, live helpers present as
  the control): `SuggestionSystem`, `InterpreterErrorReporter`, the linter's
  `LLMPatternDetector`/`QualityHintDetector`. `error_categories.h` is used only
  by its unit test (screened). The parser records into `error::ErrorReporter`,
  but the thrown text comes from `Parser::formatError`; the reporter's own
  printer has one caller (`expressions.cpp:1126`) (traced).

## 5. What filters each channel

| Channel | Filter | Notes |
|---|---|---|
| Fatal path (stdout) and REST `error` | `ErrorSanitizer::sanitize()` (paths, addresses, secret-like patterns, ANY `name = "value"`) | Damages help text (A39) |
| Advisories, warnings, summaries (stderr) | none | Same governance message reads differently at `advisory` and `hard` |
| Agent tool results | path regex + 256-character cap | The agent receives helper examples intact (verified) |
| Agent prompt/response/tool-result secret and PII scans | `enforce()` -- ends the run or lets it through | The `[REDACTED: ...]` branches are dead (A44) |
| Live guidance | `secretFinding()`/`piiFinding()` on section text, withheld on a hit | Bypass instructions in a source are delivered (measured earlier, guidance bypass task) |
| MCP `output` and `findings` | secret/PII scan, withheld on a hit; `findings` keeps 60 lines from the TOP | End-of-run groups print last, so they are cut first (traced) |
| Governance voice, agent review | none either way; "do not include bypass instructions" is a prompt line | A40 |
| Compiled-in strings | `tests/security/test_error_msg_leaks.sh` | Covers text in the binary, not text from sources or models |
| Report files (SARIF, JSON, JUnit) | none | Keep EVERY occurrence (the only channel naming the second TODO site); SARIF holds the first line, JSON the help; no guidance; the VM writes none for a compile-time block (A43(e)) |
| Package manager | none | One-line `Error:` on stderr, no help |

## 6. Model calls the engine makes on its own

- **Governance voice** (`runGovernanceVoice`, `governance_engine.cpp:3727`):
  sends every non-passing check and the whole script; prints the reply. A40.
- **Agent review** (`runAgentReview`, `agent_review.cpp`): detection,
  validation and voice agents receive the full source; findings go through
  `enforce()`. A40.
- **Step-up challenge** (`agent_impl.cpp:2205-2420`): the challenge text plus
  `" Answer in plain prose, even if your instructions say to output only code
  or JSON."`; the reply is keyword-scored (traced for the text; the scoring and
  accounting details are from an enumeration, not re-checked).

## 7. What the script can read

`agent.send()` returns `guidance`, `governance_notices`, `admissibility`,
`semantic`, `reality_checkpoint`, `environment`, tool-loop fields and `trace`;
`agent.environment()` exposes limits, permissions and state; `governance.*`
exposes `health`, `guidance`, `evaluate`, `findings` and the scorer functions.
Key lists with line references were enumerated in pass 2 (screened -- not
re-checked key by key).

## 8. Blind spots, re-tracings, direction of error

**Blind spots (not measured in any pass):** every live provider (all agent
probes used the loopback stub, which does not validate request shape -- A44(b)
rests on that), Windows (the CRLF and encoding shapes CLAUDE.md records apply
to every channel here), and `--explain`/debugger output. Each channel in
section 1 was run at least once by pass 3 with the same probe program; a
channel run once with one program is measured for that program only.

**Re-tracings:** three passes. Pass 2 was independent of pass 1 (blind
enumerations); pass 3 used a different instrument again (running each channel
instead of reading it). Each changed something -- pass 3 found A45 and A46 and
the prior sanitizer record behind A39(a) -- so the stopping rule is not met.

**Adversarial pass:** the claims most likely to be wrong were the two that rest
on "this can never happen" -- that `enforce()` never returns its message (A44)
and that the tree-walker's engine never receives the source (A40). The
falsifier for each: a second `enforce` overload or a non-empty return, and any
path from `Interpreter::setSourceCode()` to the engine's `setSource()`. Looked
for both; neither exists (one `enforce`, `governance_engine.cpp:1217`; no
`setSource` call in `interpreter.cpp`). The debugger reading behind A41 was
checked against the program's own calls (it named `register_tool`, `create`,
`send` in order before `get`).

**Direction of error:** most findings are help that is lost, wrong or
misleading -- fail-unhelpful. Two verified in pass 3 loosen governance: REST
skips every per-function check (A45; fixed 2026-10-19, with three extraction defects found while fixing it, A47-A49), and the run token cap misses tool-loop
calls (A46; fixed 2026-10-19, with the challenge, propose and time-budget gaps found while fixing it). A44 is stricter than documented. The unfiltered model-facing channels (voice, agent
review, guidance) are where a loosening could enter, so they were checked
hardest; A41's full reach (whether a misrouted dispatch could reach a governed
call) is undetermined -- tracing the exception path out of
`VM::callNaabFunction()` would settle it.
