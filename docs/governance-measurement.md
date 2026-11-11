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

## The adversarial pass

The claim most likely to be wrong was the row of zeros: secrets, cryptography and path traversal catching nothing on Juliet looks like a broken instrument. What would show that: the same rules firing when the idiom is sent alone. Looked: the three controls fire in the same run (so the checker and config are live for `no_secrets`, `restrictions.crypto` and `dangerous_calls`); the `#define` form, `CALG_3DES`/`CALG_MD5` and `rand()` sent alone fire nothing; the assignment form and `MD5()` fire. So the zeros are the checks' reach, for those two. For `no_path_traversal` it stays **undetermined**: nothing I planted fires it under this config, and the observation that would settle it is one block that does. Two re-tracings changed the text: the first draft said `dangerous_calls` separated the path-traversal variants on `fopen`, and the checker's own message says `socket(`; and it counted CWE-256 as a hard-coded credential, which it is not. A third reading changed nothing.

## The repository's own blocks

1698 blocks, 199 flagged by some rule (12%). Most fired: code_injection 56, dangerous_calls 39, no_simulation_markers 26, per_language.banned_functions 22, shell.blocked_commands 21, no_mock_data 19. Unlabelled: read as a baseline for a later run, to see what a change to a check moves.

## What this does not show

- **One config.** Every rate is under `tools/langconform/config.json`. A project's own config can switch a check off or add banned functions; nothing here is a rate for that project.
- **Text in, verdict out.** The cases go through `naab-gov check`, which reads a block's text. What the runtime stops (the sandbox, path policy, taint) is not measured here; `docs/protection-map.md` measures that.
- **C and C++ only for the security labels**, and Juliet's code is synthetic and Windows-heavy. Python, JavaScript and shell, which most NAAb blocks are written in, have no security-labelled cases yet (PR 3: OWASP Benchmark is Java, PrimeVul is C/C++; neither fills that gap, which is itself a finding for the owner).
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
```

Timings here (4 cores, the unoptimised default build): labelling 2,608 solutions 3 min 58 s; scoring 18,288 Juliet cases 4 min 16 s; scoring the EvalPlus cases 36 s.
