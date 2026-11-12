# What the block checks catch: the first measurement (Bundle 13 PR 2)

Report only. Nothing here changes a verdict, and no number here is a target. Every figure is **observed** (2026-11-11, this container, `naab-gov` built from master `6e2ce08b` plus this PR, which changes no check) unless it says otherwise; the mapping of categories to rules is **authored**. The instrument is `tools/govmeasure`; the nightly job `.github/workflows/governance-measure.yml` repeats the run and publishes the tables as an artifact.

## What was measured, and against whose labels

Each case is one block of code sent to `naab-gov check --language LANG` under `tools/langconform/config.json` (audit mode, every check on: the profile `corpus_diff.py` already uses). A rule *fires* when it is in the answer and does not also fire on a block that only assigns a number. The labels come from outside this repository:

- **NIST Juliet C/C++ 1.3** (human-written). Each single-file test case holds a flawed function and fixed ones; it becomes a *bad* case and a *good* case. 18288 cases over 17 CWEs. Comments are removed first (they say FLAW and FIX), so are the variant guards and the `main()` tail. Multi-file test cases are left out (counted by `juliet.py`).
- **EvalPlus** (model-written). The published solutions of 16 models to HumanEval, temperature 0: 2608 solutions. EvalPlus publishes no verdict with them, so `evalplus.py` makes the label the way EvalPlus does: a solution is *good* when it returns what the task's reference solution returns on every base and plus input, *bad* otherwise. 2016 bad, 592 good. 163 of 164 tasks are used (HumanEval/32 is judged by an oracle this tool does not copy); each task's reference must pass against itself and a stub returning None must fail before any solution is believed.
- **The repository's own blocks** (1698, unlabelled): the baseline the plan kept. Many are fixtures written to trip a check, so its rates say how often a rule fires on the code that exists here, not how often it is wrong.

Controls, run before any case and all fired: cpp MD5(), cpp key-shaped literal, cpp system(), python NotImplementedError, python TODO stub. A control that does not fire makes the run UNMEASURABLE (exit 2), as does a checker that cannot be read; `tests/self-audit/test_govmeasure.sh` checks both with planted failures.

## Juliet: flawed against fixed

"Mapped" is any rule `tools/govmeasure/check_map.json` expects to catch the category. The fixed variant of a Juliet file is usually LONGER than the flawed one (two or three fixed functions against one flawed), so the size rules (`max_complexity`, `max_lines`) fire more on good cases; that is the corpus's shape, not a finding about those rules.

| CWE | what | cases (bad = good) | mapped rules: bad flagged | good flagged | rules that fired (bad / good) |
|---|---|---|---|---|---|
| CWE-23 | relative path traversal | 1400 | 0/1400 (0%) | 0/1400 (0%) | max_complexity 560 / 180; dangerous_calls 560 / 0 |
| CWE-36 | absolute path traversal | 1400 | 0/1400 (0%) | 0/1400 (0%) | max_complexity 560 / 316; dangerous_calls 560 / 0 |
| CWE-78 | OS command injection | 2800 | 1792/2800 (64%) | 1120/2800 (40%) | dangerous_calls 1792 / 1120; max_complexity 1120 / 128; per_language.banned_functions 280 / 280 |
| CWE-190 | integer overflow | 2448 | no check mapped | - | max_complexity 245 / 1397; dangerous_calls 272 / 272; per_language.max_lines 0 / 5 |
| CWE-256 | plaintext storage of a password | 56 | 0/56 (0%) | 0/56 (0%) | max_complexity 2 / 56; per_language.max_lines 0 / 30 |
| CWE-259 | hard-coded password | 56 | 0/56 (0%) | 0/56 (0%) | max_complexity 0 / 28 |
| CWE-319 | cleartext transmission of sensitive data | 112 | no check mapped | - | max_complexity 112 / 112; dangerous_calls 112 / 112; per_language.max_lines 0 / 60 |
| CWE-321 | hard-coded cryptographic key | 56 | 0/56 (0%) | 0/56 (0%) | max_complexity 2 / 56 |
| CWE-327 | broken or risky cryptographic algorithm | 54 | 0/54 (0%) | 0/54 (0%) | max_complexity 54 / 54 |
| CWE-328 | weak hash | 54 | 0/54 (0%) | 0/54 (0%) | max_complexity 54 / 54 |
| CWE-338 | weak pseudo-random number generator | 18 | 0/18 (0%) | 0/18 (0%) | none |
| CWE-377 | insecure temporary file | 144 | no check mapped | - | max_complexity 0 / 2 |
| CWE-426 | untrusted search path | 112 | 112/112 (100%) | 112/112 (100%) | dangerous_calls 112 / 112; per_language.banned_functions 56 / 56 |
| CWE-476 | NULL pointer dereference | 236 | no check mapped | - | max_complexity 0 / 93 |
| CWE-506 | embedded malicious code | 90 | 36/90 (40%) | 36/90 (40%) | per_language.banned_functions 36 / 36; code_injection 36 / 36; dangerous_calls 36 / 36 |
| CWE-510 | trapdoor | 36 | 18/36 (50%) | 0/36 (0%) | dangerous_calls 36 / 36; max_complexity 36 / 34; no_hardcoded_ips 18 / 0 |
| CWE-511 | logic or time bomb | 72 | no check mapped | - | no_pii 18 / 18 |

What the table shows (each read from the rows above, then checked by sending the idiom alone to the checker):

- **The next two rows were widened on 2026-11-12 (A148); the figures in them are the measurement as it was taken.** After the change, with the built binary and each check alone: the CryptoAPI constants flag 108 of 108 flawed CWE-327/328 cases and 0 of 108 fixed ones (72 of 108 under this page's config, whose lists leave out MD2 and RC5); the macro pattern flags 112 of 112 flawed CWE-259/321 cases and 112 of 112 fixed ones, because Juliet declares the macro once above both variants -- that shows the form is read, not a catch rate. `rand()` (CWE-338) stays 0 of 18, by decision.
- **Hard-coded credentials are not caught in the form Juliet writes them** (CWE-259, CWE-321: 0 of 112 flawed cases, observed; CWE-256, a password stored in plaintext, is a different flaw the secrets check does not look for). Juliet's flaw is `#define PASSWORD "ABCD1234!"` followed by `strcpy(password, PASSWORD)`. Sent alone, the `#define` fires nothing; `const char* password = "ABCD1234!";` fires `code_quality.no_secrets`. The check reads an assignment to a credential-named variable, not a macro.
- **Weak cryptography is not caught in the Windows CryptoAPI form** (CWE-327, CWE-328: 0 of 108; CWE-338: 0 of 18). `CryptDeriveKey(..., CALG_3DES, ...)`, `CALG_MD5` and `rand()` fire nothing; OpenSSL's `MD5(...)` fires `restrictions.crypto` (the control).
- **Path traversal is not caught** (CWE-23, CWE-36: `no_path_traversal` 0 of 2,800). What does separate the variants there is `dangerous_calls` (560 flawed, 0 fixed in each, observed), which is not mapped to the category: its message names `socket(`, which the flawed variants that read the path from the network call and the fixed ones (a constant path) do not. That is the input source, not the traversal. No planted block showed `no_path_traversal` firing under this config at all, so this zero may be the config's doing (the report says so under the table it prints).
- **Command injection: the mapped rules flag 1792/2800 (64%) of flawed cases and 1120/2800 (40%) of fixed ones** (CWE-78), a difference of 24 points (observed). The fixed variants still call the same sink (`system()`, `popen()`, `execl()`), on a constant string, which a text check cannot tell from the flawed call; part of the difference is again the `socket(` the flawed variants read their input from. CLAUDE.md already states this limit of text checks. CWE-426 is the same at the extreme: every flawed and every fixed case is flagged.
- **Where the flaw is a literal, the check separates cleanly:** CWE-510 (a trapdoor reached through a hard-coded IP address): `no_hardcoded_ips` flags 18 of 36 flawed and 0 of 36 fixed.
- **Categories with no check** (CWE-190 integer overflow, CWE-476 NULL dereference, CWE-377, CWE-319, CWE-511): the only rules that fire are the size rules and `dangerous_calls` (on `socket(`, in both variants alike), the size rules more often on the longer fixed files. `no_pii` fires on 18 flawed and 18 fixed CWE-511 cases alike.

## EvalPlus: wrong solutions against right ones

The label says a solution is WRONG. That is wider than what the gaming checks look for (a stub, a placeholder, a hard-coded answer): an honest attempt with a bug is *bad* here and is not theirs to catch. So the left column is a floor on how much wrong code they stop, not a recall for "gamed" code; the right column is the one that transfers: how often they flag code that passes every hidden test.

| rule | wrong solutions flagged (of 2016) | 95% | right solutions flagged (of 592) | 95% |
|---|---|---|---|---|
| (any rule) | 218/2016 (11%) | 10-12% | 11/592 (2%) | 1-3% |
| (any mapped rule) | 149/2016 (7%) | 6-9% | 7/592 (1%) | 1-2% |
| no_oversimplification * | 92/2016 (5%) | 4-6% | 4/592 (1%) | 0-2% |
| max_complexity | 52/2016 (3%) | 2-3% | 1/592 (0%) | 0-1% |
| per_language.max_lines | 46/2016 (2%) | 2-3% | 1/592 (0%) | 0-1% |
| no_placeholders * | 26/2016 (1%) | 1-2% | 0/592 (0%) | 0-1% |
| no_hardcoded_results * | 20/2016 (1%) | 1-2% | 3/592 (1%) | 0-1% |
| no_incomplete_logic * | 19/2016 (1%) | 1-1% | 0/592 (0%) | 0-1% |
| crypto | 12/2016 (1%) | 0-1% | 3/592 (1%) | 0-1% |
| per_language.banned_functions | 4/2016 (0%) | 0-1% | 0/592 (0%) | 0-1% |
| no_hallucinated_apis * | 3/2016 (0%) | 0-0% | 0/592 (0%) | 0-1% |
| no_pii | 3/2016 (0%) | 0-0% | 0/592 (0%) | 0-1% |

\* mapped to the category. Rules that fired on fewer than 3 solutions are left out of this table (they are in the nightly artifact).

- **The gaming checks flag 7/592 (1%) of right solutions** (95% interval 1-2%) and 149/2016 (7%) of wrong ones. Of the solutions they flag, 96% are wrong; that figure depends on this corpus being 77% wrong, so it does not transfer.
- **Most wrong solutions are not stubs**, which is why the left column is small: the labeller's reasons were 1,513 wrong answers, 341 exceptions, 132 files that do not load, 30 killed at the resource limit.
- `no_placeholders` and `no_incomplete_logic` flagged no right solution (0 of 592 each); `no_oversimplification` flagged 4 and `no_hardcoded_results` 3.

Pass rates by model, as this labeller judged them (163 tasks): gpt-4 81.6%, chatgpt 65.6%, starcoder 29.4%, codegen-16b 27.6%, down to stablelm-7b 2.5%. **Not checked against EvalPlus's own published figures in this session** (they are for 164 tasks and an earlier input set); a labeller that disagreed with them by more than a few points would be the observation that shows these labels wrong. What was checked: nine planted solutions with known verdicts (`evalplus.py selftest`), the two per-task controls, and two labelled solutions read by hand.

## PR 3: the NAAb-text checks, the path-traversal zero, a second profile, and Python/JavaScript/shell labels

Observed 2026-11-11 on `24cf9d80` plus this PR's tools; no check was changed. Everything here goes through `naab-gov check` or `naab-gov lint`, which read text.

### The 15 checks lint applies to NAAb code and a run does not (A132)

`tools/govmeasure/naab_lint.py measure` lints every tracked `.naab` file twice: whole, and with each polyglot block's body emptied (the header and closing lines kept), so what still fires is firing on NAAb text -- the part a run does not check. Of 1,283 files, 1,252 were measured (30 do not parse; one more does not parse once its block bodies are emptied); 422 hold polyglot blocks. The labels are the repository's own: these are programs the project wrote and runs, so a flag on NAAb text is a false alarm unless a hand-read says otherwise. Flagged by at least one of the 15: 387 files whole, 344 on NAAb text alone (27% of the measured files).

| check | files flagged, whole | files flagged, NAAb text only | what the NAAb-text flags are (hand-read) |
|---|---|---|---|
| `no_mock_data` | 256 | 250 | 241 of 250 are an identifier starting `test_` (a test's own function); 7 `mock`/`fake`/`dummy`; 2 `foo`/`bar` |
| `no_simulation_markers` | 46 | 36 | prose in strings and comments ("simulate", "pretend") in tests that are about those words |
| `no_apologetic_language` | 29 | 29 | the pattern `oops` has no word boundary: 57 occurrences inside another word (`Loops`, `loops`) against 4 as a word; 25 of the 29 files have it only inside a word |
| `no_dead_code` | 26 | 22 | prose comments that start `// Return the ...` |
| `no_hardcoded_results` | 26 | 22 | test functions that return a literal on purpose |
| `code_injection` | 39 | 19 | prose: `eval(` inside a prompt string, "Function (" in a comment, a string ending in `for " +`, a variable named `py_rt.exec(` |
| `no_temporary_code` | 16 | 13 | comments |
| `no_hardcoded_urls` | 20 | 12 | example URLs in tests of the http module |
| `no_debug_artifacts` | 11 | 11 | `print` of a debug-looking label |
| `no_path_traversal` | 13 | 11 | `"../` inside a NAAb string literal (lint does not strip NAAb strings) |
| `no_hardcoded_ips` | 11 | 11 | addresses in SSRF tests |
| `no_sql_injection` | 6 | 4 | `"SELECT ... " + id` in NAAb (also counted under `code_injection`) |
| `no_unsafe_deserialization` | 1 | 0 | |
| `shell_injection` | 2 | 0 | |
| `banned_functions` | 0 | 0 | not a result: the measurement config holds no list for NAAb. With one, lint flags a listed call in NAAb text (probed), so its false alarms are whatever the operator lists |

What this says for the A132 decision, screened by the hand-read of the samples and not by a label: on NAAb text the three largest are pattern matches on names and prose (`test_`, `oops` inside `Loops`, a comment starting "Return"), and none of the hand-read flags was a defect in the program. The repository is mostly tests, which is where `test_` identifiers and deliberate literals live, so a product codebase would show fewer `no_mock_data` flags; the `oops` and comment-prose flags do not depend on that. The opposite direction is unmeasured: there are no labelled BAD NAAb programs, so this shows what these checks would refuse wrongly at run time and nothing about what they would catch.

(A149 was fixed on 2026-11-12: with word boundaries on the patterns this row falls from 29 files to 4, and the total on NAAb text from 344 to 336. The table above is the measurement as it was taken.) One of these is a defect a run already had (A149): `code_quality.no_apologetic_language` refuses a Python block whose only comment is `# Loops over the items` (SOFT, verified beside a control that says "Iterate"), because the same pattern list is applied to polyglot blocks.

### The path-traversal zero

Settled: it is the check's reach, not the config. `no_path_traversal` reads the block with string literals removed and looks for a literal `../` (or its URL-encoded form). It fires on a shell block's bare `cat ../../etc/hostname` and on a comment holding the sequence; it does not fire on `open("../../etc/hostname")`, where the sequence is inside a string. None of Juliet's 5,600 path-traversal cases holds a literal `../` or `..\` at all (counted): the tainted path arrives from a socket, a file or the environment. A planted shell block is now one of the controls, so the rule is shown firing in every run.

### A second profile: the config `naab-lang init` writes

A bare `{"mode": "enforce"}` flags nothing in `naab-gov check`, so the second profile is what a new project starts with (`naab-lang init`, the standard preset). Two things about it had to be found before any number could be read:

- **It allows only Python and shell.** Every Juliet case is C or C++, so each is refused as a language and nothing else is judged: Juliet under this profile is UNMEASURABLE, by the profile's design.
- **It sets checks to `hard`, and a hard block ends the scan.** The checks after the first hard finding never run, so a per-rule count says which check comes first, not which checks see the code. `eval(` is reported by `dangerous_calls` and never reaches `code_injection`. The scorer has an any-rule mode for such a config (`--any-rule-only`): only the total is reported, and a control passes when any rule flags its planted block.

| cases | profile | flawed/wrong flagged | sound/right flagged |
|---|---|---|---|
| EvalPlus, 2,608 solutions | langconform | 218/2016 (10.8%) | 11/592 (1.9%) |
| EvalPlus, 2,608 solutions | init | 202/2016 (10.0%) | 8/592 (1.4%) |
| Semgrep Python, 1,588 cases | langconform | 626/978 (64.0%) | 378/610 (62.0%) |
| Semgrep Python, 1,588 cases | init | 649/978 (66.4%) | 389/610 (63.8%) |
| Semgrep shell, 13 cases | langconform | 10/10 | 3/3 |
| Semgrep shell, 13 cases | init | 7/10 | 2/3 |

The two profiles tell the same story. The init profile flags a few more Python security cases because it switches the network off (`capabilities.network` fires on 146 flawed and 108 sound cases there).

### Python, JavaScript, shell and seven more languages: Semgrep's own test files

Juliet covers C and C++. For the languages most NAAb blocks are written in, the labels come from the Semgrep rules repository (pinned to commit `a5ad1714`): beside each rule sits a test file whose comments mark where the rule must fire (`ruleid:`) and where it must not (`ok:`). `tools/govmeasure/semgrep_tests.py` takes the rules under a `security` directory and makes one case per mark: the file's opening plus the stretch up to the next mark, with the marker comments removed; the category is the rule's first CWE. 4,206 cases, ten languages, 107 CWEs.

**Licence.** Those files are under the Semgrep Rules License v1.0 (read 2026-11-11): internal use only, no redistribution. So they are fetched at run time, the cases file is written to a scratch directory, and neither is committed, uploaded as an artifact or quoted. By the owner's conditions (2026-11-11) the published tables and the job's log carry counts only -- no Semgrep rule id, file path or code; a case's id is an opaque hash for that reason. Whether "internal use only" covers NAAb if it is ever offered commercially is an open legal question, not settled here.

Flagged by any rule, under the langconform profile (all nine controls for these languages fired):

| language | flawed stretches flagged | "must not fire" stretches flagged |
|---|---|---|
| python | 626/978 (64%) | 378/610 (62%) |
| javascript | 213/448 (48%) | 121/292 (41%) |
| ruby | 192/366 (52%) | 161/300 (54%) |
| go | 111/194 (57%) | 82/145 (57%) |
| php | 66/230 (29%) | 49/174 (28%) |
| csharp | 18/138 (13%) | 8/90 (9%) |
| typescript | 12/69 (17%) | 11/72 (15%) |
| c | 1/33 | 0/28 |
| rust | 6/20 | 3/6 |
| shell | 10/10 | 3/3 |
| **all** | **1,255/2,486 (50%)** | **816/1,720 (47%)** |

By weakness, every language together (any rule; then the rules the map names for it):

| CWE | flawed flagged | "must not fire" flagged | mapped rules: flawed / "must not fire" |
|---|---|---|---|
| 89 SQL injection | 223/297 | 156/212 | 133 / 74 |
| 79 cross-site scripting | 125/281 | 93/218 | none mapped |
| 78 command injection | 212/224 | 117/128 | 206 / 108 |
| 918 server-side request forgery | 44/162 | 24/76 | none mapped |
| 22 path traversal | 16/120 | 10/95 | 0 / 0 |
| 94 code injection | 71/114 | 53/86 | 48 / 38 |
| 95 eval injection | 75/92 | 50/60 | 64 / 47 |
| 327 broken cryptography | 59/76 | 44/61 | 39 / 28 |
| 502 unsafe deserialization | 38/75 | 24/49 | 30 / 22 |
| 798 hard-coded credentials | 49/55 | 17/25 | 11 / 3 |
| 328 weak hash | 48/60 | 7/10 | 28 / 1 |

How to read it. The text checks flag the flawed stretch and the "must not fire" stretch at nearly the same rate, in every language with enough cases to say. That is the Juliet command-injection result again, and for the same reason: `ok:` means "this rule must not fire here", usually because the argument is a constant or was sanitised, and a check that reads for the call (`os.system(`, `eval(`, `pickle.loads(`) flags the call whatever is passed to it. So this is not a count of mistakes about what the code is; it is a measurement that these checks answer "does this block use the sink", not "is this use of the sink unsafe". Three things blur it further, all in the direction of making the two columns look alike: every case carries its file's opening (the imports, which `restrictions.imports` and the URL check read), a stretch can hold more than its marked line, and 35 marks were left out (20 that Semgrep itself marks as known gaps or that label nothing, 15 whose rule names no CWE). The rows where the columns differ are the ones where the sink itself is the flaw: weak hashes (48/60 against 7/10) and hard-coded credentials. Path traversal is caught in none of the ten languages by its own rule (0 of 120), as on Juliet. Cross-site scripting and request forgery have no check that targets them; what fires there is incidental.

## The adversarial pass

The claim most likely to be wrong was the row of zeros: secrets, cryptography and path traversal catching nothing on Juliet looks like a broken instrument. What would show that: the same rules firing when the idiom is sent alone. Looked: the three controls fire in the same run (so the checker and config are live for `no_secrets`, `restrictions.crypto` and `dangerous_calls`); the `#define` form, `CALG_3DES`/`CALG_MD5` and `rand()` sent alone fire nothing; the assignment form and `MD5()` fire. So the zeros are the checks' reach, for those two. For `no_path_traversal` it stays **undetermined**: nothing I planted fires it under this config, and the observation that would settle it is one block that does. Two re-tracings changed the text: the first draft said `dangerous_calls` separated the path-traversal variants on `fopen`, and the checker's own message says `socket(`; and it counted CWE-256 as a hard-coded credential, which it is not. A third reading changed nothing.

**PR 3's pass.** The claim most likely to be wrong: "the checks flag flawed and sound Semgrep stretches alike" could be the adapter's doing -- if the cutting put the flawed line into its neighbours' cases, both columns would be the same code. What would show that: a case holding its neighbour's marked line. Looked: the adapter's self-test checks each case holds its own stretch and not the next (SG-04, SG-05), and the columns do separate where the sink is the flaw (CWE-328: 48/60 against 7/10), which a cutting error would flatten too. What the cutting cannot remove is the shared file opening, and that is stated above. A second: the A132 table's "NAAb text only" column depends on emptying block bodies correctly; a mutant that leaves them in turns three arms red (NL-01, NL-05, NL-06), and one file that stops parsing once emptied is counted as unparseable, not as clean. A re-tracing changed the write-up twice: the init profile's per-rule rows were first read as that profile's rule mix, until two controls failed and showed a hard block ends the scan; and Juliet under the init profile was first read as "0 flagged of 18,288", which is the language gate, not a result. The last reading changed nothing.

## The repository's own blocks

1698 blocks, 199 flagged by some rule (12%). Most fired: code_injection 56, dangerous_calls 39, no_simulation_markers 26, per_language.banned_functions 22, shell.blocked_commands 21, no_mock_data 19. Unlabelled: read as a baseline for a later run, to see what a change to a check moves.

## What this does not show

- **Two configs, not yours.** The rates are under `tools/langconform/config.json`, with the `naab-lang init` profile beside it for Python and shell. A project's own config can switch a check off or add banned functions; nothing here is a rate for that project.
- **Text in, verdict out.** The cases go through `naab-gov check`, which reads a block's text. What the runtime stops (the sandbox, path policy, taint) is not measured here; `docs/protection-map.md` measures that.
- **The security labels are narrow in two different ways.** Juliet's (C and C++) are synthetic and Windows-heavy. Semgrep's (ten languages) say where one rule must and must not fire, on a stretch cut from a test file: `ok:` is not "safe code", and a stretch is not a program. Neither is LLM-written. OWASP Benchmark (Java) and PrimeVul (C/C++) were not built, by the owner's decision.
- **No labelled bad NAAb programs.** The A132 table shows what the 15 checks would refuse wrongly on the repository's own programs, and nothing about what they would catch.
- **The mapping is mine.** A rule not mapped to a category is still printed when it fires, so a wrong mapping shows (CWE-23 above), but "mapped rules caught 0" is only as good as the map.
- **Direction of error.** A zero on the bad side can be the config's doing; the report names each mapped rule that no planted block showed firing. A low rate on the good side can hide a rule that never fires at all; the bad side and the controls are what show it does. The error checked harder was false reassurance: a run that scores a dead checker as "nothing flagged" (it is UNMEASURABLE instead: `test_govmeasure.sh` GM-05, and ST-09 for a checker that answers but never flags).
- **Bundle 12's control-flow bar is not answered here.** It needs NAAb programs (`tools/taintflow`).

## Reproducing

```bash
D=/some/scratch/dir
python3 tools/govmeasure/datasets.py fetch $D          # pinned by SHA-256; a mismatch is refused
python3 tools/govmeasure/juliet.py cases --zip $D/juliet-cpp-1.3.zip --out $D/juliet.jsonl
python3 tools/govmeasure/evalplus.py cases --tests $D/humanevalplus-0.1.10.jsonl.gz \
    --samples $D/evalplus-samples-*.zip --out $D/evalplus.jsonl   # RUNS model-written code, in limited children
python3 tools/govmeasure/govmeasure.py blocks --out $D/blocks.jsonl
python3 tools/govmeasure/govmeasure.py score $D/juliet.jsonl --md juliet.md --json juliet.json
# PR 3
python3 tools/govmeasure/naab_lint.py measure --md naab-lint.md              # the A132 table
python3 tools/govmeasure/semgrep_tests.py cases --repo $D/semgrep-rules --out $D/semgrep.jsonl   # never commit or upload
python3 tools/govmeasure/govmeasure.py score $D/semgrep.jsonl --group source --md semgrep-by-language.md
python3 tools/govmeasure/govmeasure.py score $D/semgrep.jsonl --group category --md semgrep-by-cwe.md
(mkdir $D/init && cd $D/init && naab-lang init)                              # the second profile
python3 tools/govmeasure/govmeasure.py score $D/evalplus.jsonl --config $D/init/govern.json --any-rule-only
```

Timings here (4 cores, the unoptimised default build): labelling 2,608 solutions 3 min 58 s; scoring 18,288 Juliet cases 4 min 16 s; scoring the EvalPlus cases 36 s.
