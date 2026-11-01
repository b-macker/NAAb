# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`naab-gov ratchet OLD NEW`: would a mid-run reload of OLD by NEW be accepted?** Exit 0 accepted, 1 refused (each violation named; a NEW whose signature does not verify is refused too, unless `--ratchet-only`), 4 a config error. It asks the engine's own reload decision (`evaluateReload()`, which the reload itself now calls), after loading OLD as a run would, with `--env`, `--agent-id` and `--lease-grant` laid over it as at startup (with no `--env`, `governance.default_env` applies, as in a run). `--json` prints the decision.
- **A property check of "governance only tightens"** (`naab_ratchet_property`, Bundle 8): generated config pairs through the real reload, with random startup overlays, judged by the engine's own gates; also that an agent role and a capability lease only narrow, and that `--sandbox-level`, `--timeout` and `--memory-limit` never raise what `govern.json` configures. It runs in the suite and, at a larger budget, in CI. Its first runs found the holes below.
- **Two fuzzers and a ThreadSanitizer CI job (Bundle 9).** `tests/fuzzing/model_channel` sends generated model replies and tool calls through the agent stub -- empty, huge, malformed, odd encodings, broken tool pairing -- and requires every run to end in exit 0, 3 or 4 with its evidence written and its telemetry chain intact. `tests/fuzzing/config` mutates a restrictive govern.json and requires it to load or exit 4, never crash, and never load weaker than written, judged by probes that try what it forbids. The `ThreadSanitizer (concurrency suites)` job (`tests/tsan/run_tsan.sh`) runs the async, agent-batch and polyglot-concurrency suites under TSan and fails on any report its suppressions file does not give a reason for.

### Fixed
- **A tool now runs on the VM its agent call came from (A93).** Every tool ran on the main VM from whatever thread the tool loop was on. So two `agent.batch` / `fan_out` workers whose models both called a tool ran on one VM stack at once: their results were wrong, and the batch result read null. A tool called from an `async fn` ran on main's stack while main was still executing, which could crash the process. A batch now lends its VM to the workers, which enter it one at a time; the model calls still run concurrently. An `async fn`'s tool runs on the `async fn`'s own VM, and function-scope capabilities attribute it to that function.
- **A restriction written in a type the loader does not read now refuses the config (A111).** `"shell": {"enabled": "no"}` (or `0`, `"off"`, `"false"`), and a `blocked_paths` or `blocked_read` entry written as a nested list, used to load without the restriction: the shell ran, and the blocked file and variable were read, under `mode: enforce`, with no warning. The same was true of a single string where `languages.allowed`/`blocked`, an agent role's lists or a function's `allowed_actions` expect a list. Such a config is now refused at load (exit 4), naming the key and the type it must be; a mid-run reload to one is refused as a config error.
  - **Migration:** a config that relied on one of these values being ignored no longer loads. Write the switch as `true`/`false` and each list as a list of strings. A single string where a `capabilities` list is expected still loads, as a one-entry list, and so do the bare forms `"shell": false`, `"network": false` and `"filesystem": "read"`. Of the 164 configs in this repository with these sections, the only one refused is a test fixture poisoned on purpose.
- **A provider's token counts can no longer un-spend a token budget (A110).** The run budget (`agent_dispatch.hard_stop.max_tokens_per_run`) and each agent's `max_total_tokens` add the counts in the response body. A reply reporting negative counts lowered the total, so a 1,000-token run kept sending; a count past `int`'s range wrapped (2^62 read as 0). Counts are now read as 64-bit and clamped to 0..10,000,000.
- **Report writers no longer read the check results while another thread records them (A86).** The JSON, SARIF, JUnit, CSV and HTML writers, telemetry and the post-run audits read `check_results_` without its lock while `async fn`s and agent workers recorded checks, which could crash the process or write a torn record. They now read a copy taken under the lock.
- **Two thread-safety defects ThreadSanitizer found on its first run (A108, A109).** The polyglot-optimization calibration was published before it was built, so a second `async fn` could read a half-built table; and the audit logger and the tamper-evident logger formatted timestamps with `gmtime()`, whose one static buffer let entries written at once carry each other's time.
- **Under enforce, a misspelled `security.sandbox_level` left the run unrestricted (A94).** The clamp to `standard` skipped any config that set a level, valid or not, so a typo kept the CLI's default. Only a real level now grants more than `standard`; a misspelled one still only warns.
- **An environment's `mode` now decides whether governance is active (A95).** `--env prod` with `mode: enforce` over a file in `mode: off` enforced nothing. And a reload could switch an active engine off without the ratchet seeing it, under an environment whose `mode` is off. Such a reload is now refused.
- **Two reloads the ratchet accepted loosened governance (A96).** With `env_vars.read: false`, dropping a name from `blocked_read` was accepted, though child processes then received it. And `detect` -> `soft` was accepted, though under `--governance-override` a soft block lets the action through. Both are refused now.
- **Two more role edits the ratchet accepted loosened governance (A101, A102).** Removing a role's last ENV action from `allowed_actions` stopped the matrix governing the environment, so the action it had denied became allowed. And removing a role's `network_allowed: false` gave the role the network back. Both are refused now, as removing `shell_allowed: false` already was.
- **An `agents` block written as a list now warns (A100).** The loader reads only the object form, keyed by role name, so a list loaded no roles at all and said nothing.
- **A capability lease no longer widens what child processes receive (A97).** A lease without ENV_READ switches the child-environment scrub to allowlist mode, which dropped the project's `blocked_subprocess_vars`. A blocked name is now withheld in either mode.
- **A mid-run reload can no longer loosen any of the 773 govern.json keys the ratchet did not compare (A104, Bundle 8).** Every key the loader reads is now classified, and the ones a reload must not loosen carry a declared order the engine enforces: the run budgets (`agent_dispatch.hard_stop.*`), `limits.array_size` / `call_depth`, `context_drift.enabled`, the circuit-breaker and exposure thresholds, `restrictions.polyglot_output`, every presence-enabled code_quality block, and the rest. Keys with no single tighter direction refuse any mid-run change. Measured on 2.0.1 with `naab-gov ratchet`: the loosening of 463 of the 475 declared keys was accepted.
  - **Migration:** a reload is now refused when it adds or removes a declared key that has no declared default; when it edits a `fixed` key (hook commands, retry tuning, state-file paths) or an existing `governance_plugins` entry; or when it rewrites a check's shorthand into an equivalent object form. Make such edits between runs. The violation names the key and its declared order.
- **A refused mid-run reload no longer changes the JSON depth limit (A106).** The loader applied `limits.data.max_json_depth` process-wide while parsing, and a reload parses before it decides, so a rejected reload to a deeper limit took effect anyway.
- **The codegen ratchet read 0 as the tightest value of five caps where 0 means no cap (A103).** `max_code_size_bytes`, `max_code_lines` and the three cumulative caps 50 -> 0 were accepted as tightenings, and `timeout_seconds` -> 0 (the 15 s fallback) was too.
- **A list could empty itself while two engines ran at once (A92).** The cycle collector marks from one VM's stack and globals but sweeps every value in the process, and clears unreachable containers it takes for cycles. A list another VM was using looked like one, so a program running an `async fn` computed with empty lists: observed, main's own list read 0 elements in 6 of 6 runs while main looped beside an async fn, and an async fn's list argument read empty in about 96% of calls while main collected. The collector now runs only when its engine is the only one executing in the process, and defers otherwise. An `async fn` therefore delays cycle collection until it finishes. `--gc-stats` now reports the engine that ran: collections run, deferred, and values collected. On the default engine it used to print the idle tree-walker's count, which was always 0.

## [2.0.1] - 2026-10-31

The first 2.x release with packages. v2.0.0 was tagged, but its macOS build failed and the release step that attaches the packages and notes never ran. Everything in [2.0.0] below applies to 2.0.1, including "Upgrading from 1.x". From 2.0 on, the Releases page carries 2.x only; every earlier version remains as a git tag.

### Fixed
- **macOS builds again.** `src/runtime/resource_limits.cpp` converted `pthread_t` to an integer with a `static_cast`, which does not compile where `pthread_t` is a pointer (macOS). It now produces the value CPython's `PyThread_get_thread_ident()` returns for the same thread, which the Python timeout hook compares it with: the address on macOS, the same integer as before on Linux.

### Added
- **A macOS build check in CI** (`ci.yml`): the release's macOS build steps plus a smoke test. Nothing built on macOS between v1.8.0 and the v2.0.0 tag, so the release was the first to find this.

## [2.0.0] - 2026-10-30

> Covers `v1.8.0..v2.0.0`: 387 commits on master, 256 of them merged pull
> requests. This section was not kept up to date between #165 and the README
> review of 2026-10-24. "Highlights since #165" below is a short list of
> user-visible features from that period, not a complete record;
> `git log v1.8.0..v2.0.0` is the complete record. 1.5.0 through 1.8.0 have no
> section in this file; their tags remain, and `git log` between them is their
> record.

### Upgrading from 1.x
A major version because these change what an existing project sees. Each is described in full below, with its remedy.
- **Two defaults now block** (Bundle 6, migration note below): built-in sequence patterns enforce at their own levels, and what a model writes is scanned for secrets. Each has a one-line opt-out that restores the old behaviour exactly.
- **Under an enforce `govern.json` every program runs on the default engine, in a CLI or worker process.** `--tree-walk` and the REPL are refused (exit 4), and REST `/execute` runs each request in a worker process.
- **`--no-governance` waives only the "no govern.json found" error.** It no longer switches off parts of a config that is found (A78), and a config that is found but does not load refuses the run (A84). A flag may tighten `govern.json`'s run limits and sandbox level, never loosen them (A79).
- **Every regular expression is matched by `naab::rx`** (A89): RE2 for patterns it reads exactly as before, QuickJS's libregexp for lookaround and back-references. Patterns are translated to keep the old dialect; a run of the whole test suite that also matched every input of 4 KB or less with the old matcher found no difference. A governance scan that runs past its step budget refuses the block (exit 3) instead of passing it.

### Changed -- defaults (Bundle 6; migration note)
Two defaults now block where they used to report or stay silent. Each has a one-line opt-out, and each opt-out is the old behaviour exactly.
- **Built-in sequence patterns enforce at their own level by default.** With `behavioral_sequences.enabled: true` and no `patterns` list, `default_pattern_enforcement` now defaults to `"declared"`: each built-in pattern blocks at the level it declares (most are SOFT: exit 3, `--governance-override` lets it through; `config_tampering` is HARD). It shipped `"observe"` (every built-in at advisory) in #182, because correcting the built-ins' step names switched them on for the first time. What changes for you:
  - `credential_harvesting` (an `env.get` of a name holding key/token/secret/password, then a network call within 5 events) now refuses the network call. A program that reads its own API key and calls out is that shape. Write `"default_pattern_enforcement": "observe"` to keep the old behaviour, or list your own `patterns`, which replace the built-ins.
  - A config that lists `patterns` is unaffected: listed patterns replace the built-ins and carry their own levels.
  - A reload from `"declared"` to `"observe"` is a ratchet violation, as before; so a running program cannot opt out mid-run.
- **New built-in pattern `credential_exfiltration`**: `env.get` of a credential-named variable, then `crypto.base64_encode`/`hex_encode`, then `agent.send` or an `http` call, at most 5 events apart, SOFT. No built-in covered a credential sent to a MODEL (`credential_harvesting` ends at a network call, and `agent.send` is not one). The README's example config wrote it as a user pattern.
- **What a model writes is scanned for secrets unless the config says not to.** An agent reply, each `agent.propose()` candidate and the arguments of a tool call the model makes are checked against the secret patterns when `govern.json` does not set `no_secrets` at all; a hit ends the run (HARD, exit 3), and the reply never reaches the program. `"code_quality": {"no_secrets": false}` (or `"restrictions": {"no_secrets": false}`) turns it off. The program's own source, its prompts and tool results stay opt-in, as before. A reload that writes `false` over the default is a ratchet violation. An `extends` child that wants it off must say so itself: a parent's `false` is not inherited, because absent in the child means the new default.

### Changed
- **Under an enforce `govern.json`, programs run on the default engine only.**
  - `--tree-walk` is refused (exit 4) before anything runs. That holds when the config's mode is `enforce`, and also when `--env` or `governance.default_env` would make it `enforce`. Audit and off still run the tree-walker.
  - REST `/execute` now runs each request in its own `naab-lang run` worker process on the default engine, as `naab-lang mcp` does. One request's state, data or crash no longer reaches the next or the server.
    - A server whose directory resolves to no `govern.json` refuses requests (exit_code 4), as a file run does, unless it was started with `--no-governance`.
    - On Windows `/execute` answers 501 ("not available on this platform yet"), like `naab-lang mcp`.
    - `naab-lang --no-governance api PORT` now parses the port.
  - The REPL (`naab-lang` with no arguments, `--repl`, code piped into either, and the `naab-repl*` binaries) does not start beside a `govern.json` in mode enforce or audit, or beside one that does not load (exit 4, as a file run). It loads no `govern.json` at all (A80): beside an enforce config, code piped into `naab-lang` read an environment variable the config blocks, and exited 0. It points to `naab-lang run`.
- **The README is restructured around what NAAb is for.** It led with the governance engine but spent over half its length on a language tour and a stdlib reference, described sequence detection in six places, and put the Quick Start at line 141. It now runs: the pitch and the hero, the Quick Start, how it works, what it stops (one table with the demo), what it does not stop, configuration, features in brief, the language in one example. The hero is a shipped program, `examples/quickstart/agent_task.naab` with its `govern.json`, that a visitor runs with `GEMINI_API_KEY=placeholder` -- the send is refused before any request is made. The language tour, polyglot details and the stdlib table moved to `docs/language-tour.md`, with new examples for SQL parameters, persistent runtimes, `naab_return()` and `-> JSON`. `tests/self-audit/test_readme_claims.sh` reads both files and now also checks the prose: the four enforcement levels, the protected policy files, shell-off, `CONTRA-013`, the MCP tool list, the quoted error messages, the pattern and signal counts, Go and PHP headers, live guidance, and response secret scanning.
- **`USER_GUIDE.md` is rewritten as a guide to working in a project.** The old guide described a different language: `import "stdlib" as std`, a block registry with "24,483 blocks", `use BLOCK-PY-09145`, a `~/naab-repl` binary, and 45 code fragments with no `main`, several of which could not parse. The new one goes from `naab-lang init` to modules, other languages and the policy, and says what a new project's `govern.json` and sandbox allow (writes, environment variables and shell blocks need `security.sandbox_level: "elevated"`). Every program in it is a complete file, and `tests/self-audit/test_readme_claims.sh` RC-22 runs each one in a fresh `naab-lang init` project with only the `govern.json` change its section shows, checking its exit code, promised values and output. It lists what does not work yet. Writing it found the defects fixed below (A71-A74, A70(a)).
- **`==` and `!=` compare lists, dictionaries and structs by value** (A72). Both engines handled only null, bools, numbers and strings and called every container unequal, so `a == a` was false for an array and `x == []` could never be true. Lists now compare element by element, dictionaries key by key, structs by type and fields, with the scalar rules unchanged (`5 == 5.0`, no conversion between types). One helper for both engines, `include/naab/value_equality.h`. Test: `tests/vm/test_value_equality.sh`.
- **README and demo checked against the binary.** Every command, config example and code block in `README.md` was run against the current build, and the claims that did not hold were corrected:
  - The opening example shows the engine's real output, and says that sequence detection needs a pattern you write and `behavioral_sequences.enabled`.
  - The CI report flags are `--governance-sarif PATH`, `--governance-junit PATH` and `--governance-report PATH` (before or after the file name since the A70(a) fix below). The old `naab-lang app.naab --governance-report sarif > results.sarif` wrote a JSON report to a file named `sarif` and the program's output to `results.sarif`.
  - The config example uses keys the loader reads (`restrictions.code_injection`, `restrictions.shell_injection`, `limits.code.max_lines_per_block`, `limits.timeout.global`). Four of its keys were read by nothing.
  - The signing commands work as written: `--keygen PATH` needs the directory to exist and writes `PATH` and `PATH.pub`.
  - Response secret scanning is described as opt-in (`code_quality.no_secrets`), BSD's 16 built-in patterns as advisory unless declared, and CDD as 23 signals.
  - Spread and rest syntax (`[...a]`, `fn(...args)`), `const` immutability and `bolo.report` are no longer listed: none of them exists.
  - The pipeline example pipes into user functions; piping into a standard-library function does not work yet (A67).
  - Added: TypeScript and SQL blocks, the MCP server, live guidance, propose/commit, output admissibility, run budgets, the `collections` module, and a "what it does not contain" note pointing at the protection map.
  - `tests/self-audit/test_readme_claims.sh` runs the README's examples and the demo, so a claim that stops holding fails the suite.
- **The demo blocks each scene for the reason it narrates.** `demo/governance_demo.sh` now requires exit 3 and the scene's own rule; scene 6 used to be blocked by the sandbox rather than by taint tracking, and scene 7 (all issues fixed) failed on two of its own checks. Re-recorded.
- **The no-govern.json error names the real requirement.** It said `--require-governance set`, a flag nobody had passed, because requiring a govern.json is the default. It now says so and points at `naab-lang init`. The `--no-governance` help line no longer claims to disable the engine (it has not since #244).

### Fixed
- **A long input can no longer crash or stall the governor** (A89). libstdc++'s `std::regex` recurses once per character, so a long enough model reply, program literal or contract pattern ended the process with a segfault (exit 139), leaving no verdict and no report; some patterns also took minutes on a long line. Every regular expression now goes through `naab::rx` (see Upgrading). The vendored QuickJS carries a backport of QuickJS 2024's regexp step limit (`external/patches/`), and the checks over polyglot blocks, function bodies, whole files, PII and model output run under one step budget. The dangerous-alias and comment co-occurrence patterns were rewritten to run in linear time without changing what they match. `--timeout` now also stops a `<<javascript>>` block stuck in a catastrophic regular expression.
- **`restrictions.dangerous_calls` no longer refuses an anonymous JavaScript function** (A91). Its patterns are matched without case, so `\bFunction\s*\(`, meant for the `Function()` constructor, also matched every `function (`. A match spelled exactly as the reserved word `function` is now skipped. Every other spelling, including an alias such as `FUNCTION(...)`, is still refused, and the rest of the table still ignores case (PHP's `SYSTEM(...)` is refused).
- **The telemetry webhook receives the whole run** (F53). Every CLI exit drains the forwarder, and `RunStart`, `RunEnd` and `GovernanceCheckSummary` are forwarded, so the forwarded chain can be verified.
- **A refused tool call is answered beside its call** (A44). A refusal used to send only a result, a request both providers reject.
- **The reasons reach MCP callers, editors and reports** (A43). A blocked `naab_propose` reports its findings, passed through the secret and PII scan; `findings` keeps the start and the end of long output; naab-lsp diagnostics name their rule again; a static HARD block on the default engine writes its reports.
- **The governance voice prints its guide once**, and a guide matching the secret or PII patterns is withheld (A40).
- **JavaScript and thrown values in VM `async fn`s** (A61, A59(a)). A `<<javascript>>` block inside an `async fn` runs; a value thrown in one reaches the `await`'s `catch` as that value, with its taint; a closure returned from one stays valid.
- **Nim and TypeScript blocks** (D12, D13). A Nim block's last line is printed when it has a value. TypeScript blocks run at `elevated`, where they ran out of memory.
- **Reported lines** (A76). Every match-level finding (secrets, placeholders, PII, incomplete logic) is printed and recorded at the line of the match.
- **Embedded Python's sockets answer to `capabilities.network`'s host lists** (A35). A `<<python>>` block's `socket.connect` was judged on the network capability alone, so `allowed_hosts`/`blocked_hosts` did not reach it, and `sendto`/`sendmsg` were not judged at all: with `network.enabled: false`, a UDP datagram left through an import the source check cannot see. Names are now judged at the lookup (`getaddrinfo`, `gethostbyname`) and at a connect given a name; when an allowed name is looked up under an `allowed_hosts` list, the engine resolves it too and admits those addresses, and a connect or send to any other address is refused unless an IP entry admits it. Alternate IPv4 spellings (`2130706433`, `127.1`) and the IPv4-mapped IPv6 form are judged as the address they reach. AF_UNIX sockets and runs with no `govern.json` are unchanged. Test: `tests/governance_v4/test_python_host_lists.sh`.
- **Threads a `<<python>>` block spawns are governed like the block** (A85). The embedded-Python audit policy read the block's sandbox and engine from thread-local state, so a `threading.Thread` the block started ran with neither: it read and deleted a `blocked_paths` file and ran `os.system` with shell disabled while the block's own thread was refused, and -- once the host lists reached Python sockets -- connected where `allowed_hosts` refused the block and sent UDP with network disabled. On such a thread the policy now uses the run's sandbox and engine, the same ones the block runs under. Tests: `tests/governance_v4/test_python_thread_policy.sh`, `tests/governance_v4/test_python_host_lists.sh` HL-15..HL-18.
- **`process.run` answers to a role's shell gate** (A54). A role with `shell_allowed: false` (or no `SHELL_EXEC` in its action matrix, or a function without `SHELL_EXEC`) ran any command through `process.run` while its `<<shell>>` blocks were refused. Test: `tests/governance_v4/test_process_run_role_gate.sh`.
- **`restrictions.information_disclosure` covers `ps -A`, `ps -e`, `ps -o pid -e` and `process.run`** (A17 F8a). `block_process_listing` matched only `ps aux` and `ps -ef` in source, and `process.run("ps", ["aux"])` was never checked. One scan now covers both, recognising every option that lists all processes, under either name (`ps`, `ps.exe`); `ps -p PID` still runs. `block_system_info_leak` reaches `process.run` too. Test: `tests/governance_v4/test_info_disclosure_commands.sh`.
- **`crypto.base64_encode`/`base64_decode`/`hex_encode`/`hex_decode` answer to `restrictions.data_exfiltration`** (F19), as `base64.b64encode(` and `binascii.hexlify(` in a block already did, at the restriction's level. Test: `tests/governance_v4/test_native_encoding_exfil.sh`.
- **A risk budget that overshoots zero is exhausted, not unlimited** (A68). Two charges on one turn took a budget of 1 to -1, the "no budget" sentinel, and every later send was admitted; `agent.environment()` reported -1. The remaining budget now stops at 0. Test: `tests/governance_v4/test_risk_budget_overshoot.sh`.
- **`agent_review` no longer fails open on an unrecognised setting** (A29). `fail_policy` is compared without case, and a value other than `open`/`closed` is treated as `closed`, with a warning. An enforcement zone level other than `advisory`/`soft` blocks as hard: an unknown string with a warning, `detect` and `approval_required` because nothing can catch a block before the program runs. Before, every value but `hard`/`soft` ran as advisory while the banner printed the configured string. Under `fail_strategy: "continue"`, a failed agent the review continues past is now reported. Test: `tests/governance_v4/test_agent_review_settings.sh`.
- **A tainted assignment meets a `variable.assign` taint sink on the default engine** (A65). The check looked the variable's name up in the engine's shared taint set, which the VM writes only for globals and only afterwards, so a local's assignment passed. Test: `tests/governance_v4/test_taint_assign_sink.sh`.
- **`naab.toml` values that read as security settings say they are not applied** (A28). `[polyglot] python = false` and `[features] sandbox_mode = false` changed nothing, silently. The manifest stays package metadata; `naab-lang run` and `naab-lang manifest check` now name the `govern.json` key that does the job (`languages.blocked`, `security.sandbox_level`). Test: `tests/cli/test_manifest_unapplied.sh`.
- **A `govern.json` that is found and does not load refuses the run, even with `--no-governance`** (A84). A parse error, an empty file, an `extends` that does not resolve or an array over the element cap read as "no govern.json found": the refusal named no config, and `--no-governance` ran the program with none of the policy (observed: a blocked environment variable printed, exit 0, on both engines). Such a run now exits 4 and says the config did not load; an integrity failure keeps exit 3. REST `/check` answers 400 instead of a verdict, `naab-gov check` exits 4, and the REPL does not start. Test: `tests/cli/test_malformed_config.sh`.
- **The default engine writes the audit trail's polyglot entries and counts every runtime `.exec()`** (A81). Only the tree-walker did either: on the VM, `audit.log_events.polyglot_executed` and `polyglot_timing` wrote nothing, and a persistent runtime's `.exec()` was not counted against `limits.execution.polyglot_blocks`, so three `js.exec()` calls ran under a HARD limit of 2. With the tree-walker refused under enforce, an enforce run had recorded no polyglot execution at all. `.exec()` entries and messages now name the call's own line; seven other VM sites still read the line three instructions back (A82, open).
- **Only the private key that signed `govern.json` unlocks a flag the owner locked** (A77). The check read the `NAAB_SIGNING_KEY` file as a public key: the owner's private key never worked, and with a signature written without a timestamp (still accepted) the public key, which anyone can read, unlocked `integrity.blocked_flags`. Test: `tests/security/test_flag_authority.sh`.
- **`--no-governance` no longer switches off parts of a `govern.json` that is found** (A78). With a config found, it still skipped the sandbox's capability sync (shell, network, environment), the enforce-mode default sandbox, the post-run audit, the `must_produce` contracts and the check for a deleted `govern.json`. It now only waives the requirement when no `govern.json` is found, as `--help` already said.
- **A flag may only tighten `govern.json`'s run limits and sandbox level** (A79). `--timeout` and `--memory-limit` can lower a configured limit, not raise it; an explicit 30-second limit counts as configured; an inherited (`extends`) limit applies as written; the tree-walker reads both limits; the limits reach child processes whether or not the sandbox level changed; and under `enforce` a `--sandbox-level` above `standard` is lowered to `standard` unless `govern.json` sets one.
- **Default parameter values on the VM** (A71). The default engine never applied them: a parameter left out of a call read whatever the stack held next, so `fn c(x, y = 10)` gave `c(1)` = 2 and `fn d(n = 5) { let t = 100; return n }` returned 100. Defaults are now evaluated on every call, in order, in the callee's scope, as on the tree-walker, and a required parameter after a defaulted one is still required. Test: `tests/vm/test_default_params.sh`.
- **The `standard` sandbox refused a new file at the top of the project** (A73). `file.write("out.txt", ...)` and `file.exists("missing.txt")` stopped the program with a sandbox violation while `data/out.txt` was accepted: a bare name for a file that did not exist yet was never made absolute. Test: `tests/security/test_sandbox_relative_path.sh`.
- **A `govern.json` run time limit below 30 seconds was raised to 30** (A74). The configured limit now applies as written. `--help` described `--timeout` as a per-block timeout. Removing the floor exposed that an `extends` merge never reached the run's limit (it set `limits.timeout.global` but not the copy the run reads); an inherited limit now applies as written. Test: `tests/cli/test_timeout_config.sh`.
- **Seven run options worked only after the file name** (A70(a)): `--governance-report`, `--governance-sarif`, `--governance-junit`, `--governance-record-baselines`, `--governance-check-baselines`, `--memory-limit` and `--allow-network`. Written first they were taken for the command ("Unknown command"). Test: `tests/cli/test_flag_position.sh`, whose FP-S arm fails on the next option added to one parse loop only.
- `--help` lists `naab-lang init`'s `--taint` and `--languages`.
- **A project made with `naab-lang init` could not run hello world.** With the default `standard` preset (and `relaxed`), the generated `govern.json` set `complexity_floor` to `"level": "off"`. `"off"` is not a level, so the check ran at hard and refused `main` for a complexity score below 10: the README's own hello world exited 3. More generally the generator wrote checks its profile meant to switch off in forms the loader ignores or enforces anyway (`"enabled": false` on a check that a present block switches on, `"level": "off"`), plus keys nothing reads. It now leaves out what its profile turns off, and each preset runs hello world with no "has no effect" or "not a setting" warning (`tests/cli/test_init_governance.sh` Test 10).
- **Optional chaining on null on the VM** (`a?.b`, `a?.m()`). The null path popped one value too many, so the expression took the value below it: `print(u?.name)` failed with "Value is not callable", and `let n = u?.name` set `n` to the print builtin. Wrong since optional chaining was added; the tree-walker was correct. Test: `tests/vm/test_optional_chaining.sh`, plus a differential probe.
- **Rule and setting names are no longer redacted in error messages.** The error sanitizer took any word starting with `token`, `secret`, `credential` or `auth` for a secret, so a rule named `credential_exfiltration` printed as `<redacted>` and `max_tokens_per_run` as `max_<redacted>`. A separator (`:`, `=`, whitespace or a quote) between the keyword and the value is now required. `token=value` without quotes is now redacted, which it was not before.

### Highlights since #165 (not a complete list)
- `naab-lang mcp`: governed execution over the Model Context Protocol, one worker process per call (#307).
- Live guidance: project knowledge from markdown, SQLite, https, command and MCP sources, delivered when governance observes the matching event (#309, #310, #312, #316).
- `<<sql>>` blocks: in-process SQLite with no filesystem reach, variables bound as parameters (#291). TypeScript blocks run through `tsx`.
- Function-scope capabilities (`capabilities.functions`): which actions a function may take, intersected down the call stack.
- `capabilities.shell.enabled: false` refuses every language that runs as a separate process (#298).
- The protection map: per language and forbidden action, which layer stops it (#296).
- The run's evidence files are protected from the governed program (#314), and a reload cannot move or switch them off (#317).
- `limits.array_size` applies at every way an array grows (#335).
- `string.slice`, and one implementation of slice/substring/replace for both engines (#262).

### Added
- **Output admissibility: the `undetermined` verdict** (`circuit_breaker.output_admissibility.on_undetermined`)
  - A gate that passes because it has no basis to judge now says so. Inside the adaptive baseline window every statistical signal's penalty is suppressed, so coherence sits at its ceiling and the threshold comparison passes on a number that was never a judgement.
  - `OUTPUT_ADMISSIBILITY_EVAL` gains `result: "undetermined"`, `baseline_state` (`calibrating`/`complete`/`disabled`) and `on_undetermined`; `OUTPUT_INADMISSIBLE` gains `undetermined`; `response.admissibility.undetermined` exposes it to scripts on both directions.
  - One-directional by design — it marks the PASS direction only. Objective signals (S1 `circular_actions`, S21 `response_repetition`) are exempt from baseline absorption and charge from turn 1, so the gate can still rule OUT while calibrating.
  - Policy: `"pass"` (default, no behaviour change), `"quarantine"`, `"block"`. Ratchet-enforced (`pass` < `quarantine` < `block`).
  - Undetermined holds never advance the quarantine streak: `adaptive_baseline_window` and `max_quarantine_streak` both default to 5, so counting them would terminate every agent on its last calibrating turn regardless of behaviour.
- **Behavioral Sequence Detection (BSD)** — FSM-based multi-step attack pattern matching
  - Configurable via `behavioral_sequences` in govern.json. Your patterns replace the built-in set (17 patterns today, enforcing at their own levels unless `default_pattern_enforcement` is `"observe"`; advisory until Bundle 6)
  - Detail globs for per-argument matching (e.g., `env.get:*SECRET*|*TOKEN*`)
  - Decay/expiry — events older than N turns or N seconds don't match
  - Pre-execution blocking — dangerous sequences blocked before the final step runs
- **Context Drift Detection (CDD)** — coherence monitoring for LLM agent conversations
  - Tracks declared intent vs observed actions across agent turns
  - Signals: repeated failures, circular actions, scope creep, contradictions
  - Configurable via `context_drift` in govern.json with per-signal weights and coherence threshold
- **VM taint lineage tracking** — every tainted value carries a source-to-sink chain
  - Preserved across `OP_RETURN` frame eviction
  - Per-language taint sinks (`python_exec`, `go_exec`, etc.)
  - Makes governance violations actionable — users see exactly where taint originated
- **govern-template.json sync** — 15 missing keys added, 11 dead fields annotated with comments explaining what replaced them, template restructured into 12 logical groups
- BSD/CDD dashboard telemetry — `--governance-dashboard` now shows BSD events processed, patterns matched, and CDD coherence state

### Fixed
- **Zero-warning build** — root-cause analysis and fix for all 20 build warnings across 8 files
  - `executeWithReturn()` missing exit code check — garbage stdout parsed as return value on subprocess failure
  - Dead code from refactors: `last_line_idx` (cpp_executor_adapter), `blocks_found` (block_registry), `DEFAULT_JSON_DEPTH` (json_impl)
  - Sign conversions in backward loops and timeout parsing (main.cpp, agent_impl, json_result_parser)
  - `std::result_of` → `std::invoke_result_t` migration (deprecated C++17, removed C++20)
- BSD/CDD correctness — 6 post-review findings (detail_glob matching, pre-execution abort, hard enforcement)
- `behavioral_sequence.cpp` moved to `naab_security` CMake library (fixed linker errors on Sanitizers, Windows, and CodeQL CI)
- Shell polyglot test T6 skipped on Windows (no shell executor on that platform)
- Stale regex docs synced with actual patterns
- Struct helper error message improved

### Security
- **V-RCE-014**: C++ polyglot `#include` path traversal bypass — relative paths (`../../../etc/shadow`) not blocked by `isCppSourceSafe()` (self-audit v14)

## [1.4.0] - 2026-04-30

### Added
- **`use agent` stdlib module** — governed LLM conversations with multi-provider support
  - `agent.create(name)` / `agent.send(handle, message)` / `agent.run(handle, message)` — full conversation lifecycle
  - `agent.messages(handle)` / `agent.usage(handle)` — conversation history and token tracking
  - Anthropic (Claude) and Gemini provider support with automatic API format translation
  - Per-agent governance: `max_turns`, `max_tokens`, `max_total_tokens`, `system_prompt`
  - Per-agent sandbox config: `allowed_paths`, `blocked_paths`, `shell_allowed` (advisory, pending tool execution)
  - Server-side `AgentTracker` — turn/token enforcement immune to handle mutation attacks
  - Output content filtering: `checkSecrets()` (18 patterns) and `checkPii()` (5 patterns) on all LLM responses
  - Tool-use response blocking — agents cannot invoke function calls (defense-in-depth until tool loop is implemented)
  - Forged handle rejection — unknown handle IDs blocked at send time
- `\x` hex escape (`\x1b` etc.) and `\e` ESC escape in string literals
- **Gate 11b**: main{} body hash — blocks modification without re-baselining
- Banned function alternatives in governance errors (eval, Function, exec, compile)
- Blocked import alternatives in governance errors (os, subprocess, shutil, socket, ctypes, pickle)
- `array.get(index)` safe access — returns null instead of throwing on out-of-bounds
- `format_timestamp()` stdlib helper
- `PROJECT_SETUP.md` — end-to-end project bootstrapping guide (keys, signing, govern.json)
- `CLAUDE.md` — internal reference for Claude Code (build, test, architecture, conventions)
- 5 governance hardening fixes from naab-15 Synapse adversarial analysis
- 5 systemic runtime enforcement fixes for LLM code generation

### Fixed
- Dict.get() hint dedup — no longer spams repeated hints in loops
- Taint labels show "argument N of 'func()'" instead of cryptic `__argN__`
- Struct-to-dict conversion error now suggests `to_dict()` or field access
- CLI arg passthrough: NAAb flags no longer consumed after script filename
- `--sandbox-level` and `--gc-stats` flags restored after passthrough regression
- `env.get_args()` returns empty in VM — args provider not wired to VM
- `allowed_paths` now takes priority over `blocked_paths`
- Sanitizer warning deduplication
- Unsigned governance mode warnings removed (broke CI tests)
- `validate_` sanitizer taint clearing + `file.append` registered as taint sink

### Security
- Agent output filtering blocks secrets (API keys, tokens) and PII (SSN, CC numbers) in LLM responses
- Server-side agent tracking prevents governance bypass via handle dict mutation
- Bypass flags removed from all governance error messages (11 files) — enforced by `test_error_msg_leaks.sh`
- 10 governance gap fixes from context-review Gemini session scanning
- 15+ DX improvements from context-review analysis (better error guidance without leaking internals)

## [1.1.0] - 2026-04-23

### Added
- **Ed25519 trust-anchored governance signing (V-SC-009)** — asymmetric signing replaces HMAC-SHA256
  - `--keygen [path]` generates Ed25519 keypair, installs public key to trust store
  - `--signing-key <path>` specifies private key for signing operations
  - `--trust-key <pubkey.pem>` installs a public key to the trust store
  - `--list-keys` displays trusted key fingerprints
  - External trust store at `~/.naab/trusted-keys/` (outside project sandbox)
  - Signature format: `ed25519:<base64>` prefix for Ed25519, `hmac:<hex>` for tagged HMAC
  - Legacy HMAC backward compatibility preserved with deprecation warning
- **CryptoUtils Ed25519 primitives** — `ed25519Keygen`, `ed25519Sign`, `ed25519Verify`, `ed25519Fingerprint`, `toBase64`, `fromBase64`
- **TrustStore class** — `trust_store.h/cpp` for managing Ed25519 public keys

### Security
- **V-SC-009: Trust store keys + missing .sig = BLOCK** — attacker sandboxed to project directory cannot bypass governance by deleting `.sig` files when trusted keys exist in `~/.naab/trusted-keys/`
- **V-SC-009: HMAC .sig + trust store = BLOCK** — forces re-signing with Ed25519 when trust store is active
- **V-SC-009: ed25519Verify rejects private keys** — only public keys accepted for verification
- **V-SC-009: readSigningKey bounded read** — lstat + S_ISREG + 8KB cap prevents abuse
- **V-SC-009: .sig file read capped at 4KB** — prevents memory abuse via crafted signature files
- **V-SC-009: TOCTOU eliminated** — .sig opened directly, branch on is_open() instead of exists-then-open
- **V-SC-009: Private key written via writeFileSecure()** — O_CREAT|O_WRONLY|O_EXCL with mode 0600
- `NAAB_SIGNING_KEY` added to blocked env vars (env_impl.cpp) and subprocess scrubbing (subprocess_helpers.cpp, persistent_process_executor.cpp)

### Changed
- `signFile()` prefers Ed25519 (NAAB_SIGNING_KEY) over HMAC (NAAB_GOVERN_KEY) with deprecation warning
- `verifySignatureImpl()` unified verification replaces duplicated logic in verifyFileSignature/verifyContentSignature
- Owner-awareness messages updated: ~15 occurrences changed from HMAC-specific to generic signing references
- govern-template.json updated: integrity comment changed from HMAC to Ed25519, `require_signature` field removed

## [1.0.0] - 2026-04-14

### Added
- array.map()/filter()/reduce() aliases (no _fn suffix needed)
- Module name aliases (fs→file, os→env, re→regex) with "Did you mean?" hints
- spawn/wait/wait_all → async function hints for common concurrency patterns
- Package integrity verification (SHA-256 hashes in naab.lock)
- Unpinned git dependency security warnings

### Fixed
- Tree-walker timeout on Android/Termux (global_shutdown_ set from timer thread)
- Package manager shell injection (system()→fork/execvp)
- Package manager silent failures (installAll reporting with summary)
- Subcommand flag parsing (naab install --verbose no longer treated as package name)
- 6 parser expect() messages upgraded with examples and suggestions

### Security
- V-PKG-001: Shell injection in extractTarball eliminated (fork/execvp)
- V-PKG-002: SHA-256 integrity hash verification for downloaded packages
- parseSpec validates against shell metacharacters (';|`$()&)

## [0.9.0] - 2026-04-09

### Added
- **String interpolation** — `f"Hello {name}"` and `"Hello ${name}"` syntax
- **for-in with index** — `for (i, item) in arr { }` binds both index and value
- **switch keyword** — alias for `match` expression
- **--quiet/-q flag** — suppresses governance info messages for scripting use
- **--strict flag** — alias for `--strict-types`
- **Multi-line error context** — parser errors show 2-3 source lines with caret
- **Parser error recovery** — reports multiple errors instead of stopping at first
- **Top-level runtime declarations** — `runtime py = python.start()` at file scope
- **Pre-built release binaries** — Linux (x86_64, aarch64), macOS (arm64), Windows (x86_64)
- **INSTALL.md** — installation guide (binary download and build from source)
- **naab-gov CLI** — standalone governance linting and scanning tool

### Fixed
- **SQLite NULL column crash** — `safeColumnText()` guards all `sqlite3_column_text()` calls
- **Thread pool double-init race** — replaced double-check pattern with `std::call_once`
- **Module cache thread safety** — added `std::shared_mutex` for concurrent access
- **popen FILE* leak** — wrapped in RAII `unique_ptr<FILE, pclose>`
- **Governance telemetry file leak** — RAII wrapper for fopen/flock/fclose
- **Debugger source locations** — `CallFrame.source_location` now uses real file:line info

### Security
- 60+ security fixes across R5-R24 audit rounds
- Windows subprocess hardening (Job Objects, handle narrowing, Ctrl-C handler)
- Lockfile HMAC-SHA256 signatures, fail-closed verification
- REST API per-request timeout, body size limits, constant-time auth
- Environment variable injection blocking (LD_PRELOAD, PATH, etc.)

## [0.8.1] - 2026-04-03

### Fixed
- **Windows CI** — `vm.cpp` `OP_POLYGLOT` handler preamble/postamble now guarded with `#ifdef HAVE_PYBIND11`, fixing Python `-> JSON` blocks in subprocess mode (Windows/no pybind11)
- **Version sync** — all version strings across configs, docs, tests, and tooling synchronized to 0.8.1
- **CI hardening** — `ci.yml` now runs full `run-all-tests.sh` suite with pybind11 installed

## [0.8.0] - 2026-04-02

### Added
- **Windows portability** — full MSYS2/MinGW64 build support, `platform_win32.cpp`, `CreateProcess` subprocess executor, GitHub CI for Windows
- **LSP enhancements** — codeAction, workspaceSymbol, rename refactoring (Sprint 8.3)
- **Deterministic builds** — `naab.lock` lockfile via `--lock`/`--lock-check`/`--lock-path` flags (Sprint 8.4)
- **Enterprise governance** — exit codes (0-4), quality gates, CWE/OWASP tags, baselines, environment selector, runtime versions
- **New stdlib modules** — `log`, `uuid`, `validate`, `process`
- **libnaab** static library and `naab-gov` standalone CLI

## [0.7.0] - 2026-03-30

### Added
- **Bytecode VM** — stack-based bytecode compiler and virtual machine, now the default execution engine
  - Computed goto dispatch (GCC/Clang) for fast opcode handling
  - NaN-boxing integer fast paths for arithmetic without unboxing
  - Constant folding optimization pass in the compiler
  - 47 VM bugs fixed during development and hardening
  - Use `--tree-walk` flag to fall back to the legacy AST interpreter
- **Agent Governance Runtime** — multi-agent role enforcement for govern.json
  - `--agent-id <name>` CLI flag to identify the executing agent
  - `--governance-dashboard` flag for governance summary output to stderr
  - `agent_roles` section in govern.json for per-agent path and language restrictions
  - Telemetry JSONL output for agent execution tracking
- **Governance Plugins** — extend governance with custom NAAb-based check functions
  - `governance_plugins` array in govern.json references external .naab files
  - Each plugin defines rules with `id`, `function`, `trigger`, `level`, and help text
- **Package Manager** — dependency management for NAAb projects
- **Multi-Agent Integration Tests** — end-to-end tests for agent governance scenarios

### Changed
- VM is now the default execution engine (`global_use_vm = true`); tree-walker available via `--tree-walk`
- NaN-boxing migration complete — zero `fromLegacy()`/`toLegacy()` calls remain outside core naab_val.cpp/h

### Fixed
- 47 VM bugs across phases 1-16 (control flow, closures, polyglot dispatch, governance integration)
- Integer overflow in array indexing with large indices
- Parallel polyglot race conditions in VM execution path
- Gorilla test #12 fixes for edge-case governance interactions

### Performance
- VM 50ms vs Tree-walker 395ms (87% faster, ~8x speedup) on standard benchmarks
- NaN-boxing eliminates heap allocation for int/double/bool/null values

## [0.6.0] - 2026-03-21

### Added
- **f-string syntax** — `f"hello {name}"` as shorthand for `"hello ${name}"`
  - Bare `{expr}` in f-strings is equivalent to `${expr}` in regular strings
  - Regular strings still require `${}` — bare `{` is literal
  - Supports arbitrary expressions: `f"result: {2 + 3}"`
  - `${}` also works inside f-strings for backward compatibility
- 6 f-string tests in `test_stdlib_string.naab` (44 → 50 assertions)
- **Pattern matching enhancements** — guard clauses, binding patterns, array destructuring
  - Guard clauses: `n if n > 0 => "positive"` — condition checked after pattern match
  - Binding patterns: `other => f"got: {other}"` — identifier binds the subject value
  - Array destructuring: `[x, y] => f"point: {x}, {y}"` — match and destructure arrays
  - Literal array elements must match exactly: `[0, 0] => "origin"`
  - Combined: `[x, y] if x + y > 25 => "far"` — destructure + guard
- 9 match enhancement tests in `test_control_flow.naab` (39 → 48 assertions)
- **Interfaces / traits** — declare interface contracts and validate struct implementations
  - `interface Printable { fn to_string() -> string }` — define required method signatures
  - `struct Point implements Printable { ... }` — declare interface compliance
  - Multiple interfaces: `struct Box implements Printable, Measurable { ... }`
  - Method validation: missing methods produce clear error messages with examples
  - Duck typing via `fn StructName.method(instance)` free functions
  - `StructName.method(instance)` call syntax for method dispatch
- 10 interface tests in `test_interfaces.naab`
- **Generators / yield** — generator functions with lazy iteration via `yield` keyword
  - `yield value` inside a function makes it a generator (auto-detected)
  - `for x in generator_fn(args)` iterates over yielded values
  - Fibonacci, filtering, flattening patterns all work
  - `break` in for-in loop stops iteration early
  - Empty generators (no yields executed) produce zero iterations
  - Nested yield: generators can yield from nested loops
- 12 generator tests in `test_generators.naab`
- **Stdlib completion** — new functions across file, http, path, and io modules
  - `file.copy(src, dst)`, `file.move(src, dst)`, `file.size(path)` — file operations
  - `file.basename(path)`, `file.dirname(path)`, `file.extension(path)` — path info from file module
  - `http.head(url)`, `http.patch(url, body)` — additional HTTP methods
  - `io.input(prompt?)` — read line from stdin with optional prompt
  - New `path` module: `join`, `dirname`, `basename`, `extension`, `resolve`, `is_absolute`, `normalize`, `exists`
- **REPL** — interactive Read-Eval-Print Loop
  - `naab-lang` with no arguments starts the REPL; also `naab-lang --repl`
  - Expressions auto-print: `3 + 4` shows `7`
  - Variables, functions, structs persist across lines
  - Multi-line input: open braces/brackets continue to next line (`...` prompt)
  - Meta-commands: `.help`, `.clear` (reset environment), `.exit`
  - Top-level declarations (`fn`, `struct`, `enum`, `use`, `import`) work correctly
  - History saved to `~/.naab_history`
- **URL imports** — import NAAb modules directly from URLs
  - `import "https://example.com/lib.naab" as mylib` — download and cache
  - Cache stored in `~/.naab/cache/` with content-hashed filenames
  - Cached modules are reused across runs (no re-download)
  - Clear error messages on download failure

### Fixed
- **BUG-E1: Type checking bypass via `callFunction()`** — pipeline (`|>`), stdlib callbacks (`array.filter_fn`), lambda calls, and method dispatch now enforce parameter type annotations. Previously only direct `fn(args)` syntax checked types.
- **BUG-E1b: Generator parameter type bypass** — generator functions now validate parameter types at creation time, before eager body execution.
- **BUG-E2: Generator exception safety** — if a generator body throws, interpreter state (`active_generator_`, `current_env_`, `returning_`) is now properly restored via try/catch. Previously, an exception mid-generation corrupted state for subsequent operations.

### Hardening
- **229 new edge-case assertions** across 9 test files targeting bypass paths, state consistency, and cross-feature interactions:
  - `test_type_enforcement.naab` (41 tests) — variable/param/return type checking, nullable, union types, struct types, error messages
  - `test_type_bypass.naab` (25 tests) — pipeline, lambda, callback, method dispatch type enforcement (BUG-E1 verification)
  - `test_generator_state.naab` (17 tests) — exception recovery, nested generators, reuse independence, break/continue state
  - `test_interface_invariants.naab` (25 tests) — multi-struct, multi-interface, error paths, method as callback, f-string interaction
  - `test_match_hardened.naab` (30 tests) — guard evaluation order, destructuring edge cases, guard+destructure combos, nested match
  - `test_fstring_hardened.naab` (26 tests) — expression types, special values, mixed syntax, complex interpolation chains
  - `test_stdlib_path.naab` (30 tests) — all 8 path functions, edge cases (root, hidden files, no extension), error handling
  - `test_stdlib_file_new.naab` (20 tests) — copy/move/size/basename/dirname/extension with real filesystem operations
  - `test_v060_interactions.naab` (15 tests) — typed generators, interface+generator, match+generator, f-string everywhere, full feature pipeline

## [0.5.3] - 2026-03-21

### Added
- **Optional chaining (`?.`)** — safely access properties/methods of potentially null objects
  - `user?.name` returns null if user is null, otherwise returns `user.name`
  - `obj?.method()` returns null if obj is null, otherwise calls the method
  - Chains: `a?.b?.c` propagates null through the chain
  - Combines with `??`: `user?.name ?? "default"` for fallback values
  - Works with structs, dicts, arrays, strings — any member/method access
- 9 optional chaining tests in `test_structs_enums.naab` (25 → 34 assertions)
- **Destructuring assignment** — extract values from arrays and dicts into variables
  - Array: `let [a, b, c] = [1, 2, 3]` — positional extraction
  - Dict: `let {name, age} = get_user()` — key-based extraction
  - Partial: extra array elements ignored, missing dict keys become null
  - Value semantics preserved (destructured values are copies)
  - Taint propagation supported for governance
- 8 destructuring tests in `test_closures_scope.naab` (30 → 38 assertions)
- **Spread/rest operator (`...`)** in array destructuring
  - `let [first, ...rest] = [1, 2, 3, 4]` — `rest` becomes `[2, 3, 4]`
  - `...rest` must be the last element in the pattern
  - Empty rest produces `[]` when array is exactly consumed
  - Value semantics preserved (rest array is an independent copy)
- 5 spread tests added to `test_closures_scope.naab` (38 → 43 assertions)
- **For loop destructuring** — iterate with pattern matching
  - `for [key, val] in dict { }` — destructure key-value pairs from dicts
  - `for [a, b, c] in array_of_arrays { }` — destructure nested arrays
  - `for [first, ...rest] in rows { }` — spread in loop patterns
- 5 for-loop destructuring tests in `test_control_flow.naab` (34 → 39 assertions)
- **Dict dot-methods `entries()` and `merge()`** — added to primary CallExpr dispatch path
  - `d.entries()` returns array of `[key, value]` pairs
  - `d.merge(other)` merges another dict into `d`, overwriting existing keys
  - `for [k, v] in d.entries() { }` — full destructuring support
- 6 dict method tests in `test_structs_enums.naab` (34 → 40 assertions)
- **Null coalescing assignment (`??=`)** — assign only if variable is currently null
  - `x ??= "default"` assigns `"default"` only when `x` is null
  - Falsy values (`false`, `0`, `""`) are NOT null — assignment is skipped
  - Supports variables and member access targets (`obj.field ??= val`)
  - Desugars to `x = x ?? val` — no new AST node needed
- 6 nullish assignment tests in `test_operators_matrix.naab` (62 → 68 assertions)
- **Compound subscript assignment** — `arr[i] += val`, `dict["key"] -= val`
  - All 5 compound operators work: `+=`, `-=`, `*=`, `/=`, `%=`
  - `??=` also works on subscripts: `arr[0] ??= "default"`
  - Supports both literal and variable indices
- 7 compound subscript tests in `test_operators_matrix.naab` (68 → 75 assertions)
- **`in` containment operator** — check membership in dicts, arrays, and strings
  - `if "key" in dict { }` — check if key exists in dict
  - `if item in array { }` — check if item is in array
  - `if "sub" in string { }` — check if substring exists
  - Returns boolean, works with `!` for negation: `if !("x" in d) { }`
- 8 `in` operator tests in `test_operators_matrix.naab` (75 → 83 assertions)
- **String repetition** — `"abc" * 3` → `"abcabcabc"`
  - Commutative: `3 * "abc"` also works
  - `"x" * 0` → `""`, `"" * 5` → `""`
- 5 string repeat tests in `test_operators_matrix.naab` (83 → 88 assertions)
- **`not in` operator** — readable negation of containment check
  - `if 99 not in arr { }` — equivalent to `!(99 in arr)` but more readable
  - Works with arrays, dicts, and strings
  - Desugars to `UnaryExpr(Not, BinaryExpr(In, ...))` at parse time
- 6 `not in` tests in `test_operators_matrix.naab` (88 → 94 assertions)
- **Array/string slicing** — `arr[start:end]` syntax for extracting sub-sequences
  - `arr[1:3]` returns elements at indices 1 and 2 (end exclusive)
  - `str[0:5]` returns substring from index 0 to 4
  - Negative indices: `arr[-2:5]` counts from end
  - Empty slice when start >= end: `arr[3:3]` → `[]`
  - Desugars to `__slice(collection, start, end)` builtin
- 8 slicing tests in `test_operators_matrix.naab` (94 → 102 assertions)

## [0.5.2] - 2026-03-21

### Added
- **Exhaustive Robustness Test Suite** — 8 test files, 308 assertions covering core language correctness
  - `test_stdlib_array.naab` (40) — all 16 array functions + empty/error edge cases
  - `test_stdlib_string.naab` (44) — all 19 string functions + format/fmt + boundary/error cases
  - `test_stdlib_math_json.naab` (49) — math (15), json (all 6), regex (all 12) including advanced
  - `test_operators_matrix.naab` (62) — all 17 operators × type matrix, truthiness, ??, |>, string comparison
  - `test_closures_scope.naab` (30) — capture, factory, nested, shadow, IIFE, higher-order
  - `test_control_flow.naab` (34) — nested break/continue, return in loops/try, match, empty iter
  - `test_structs_enums.naab` (25) — struct create/modify, enum match, value semantics
  - `test_stdlib_env_time.naab` (33) — env (all 12 fns), time (all 12 fns) including parse_datetime
- **Meta-Test Validation Suite** — 5-layer system proving test assertions are genuine
  - Layer 1: Static integrity audit (7 checks: no trivial assertions, balanced counts, no orphans)
  - Layer 2: Mutation testing — 45 deliberately wrong assertions, all correctly detected
  - Layer 3: Sensitivity testing — 33 tests proving outputs depend on inputs
  - Layer 4: Coverage verification — cross-references stdlib C++ source against test usage (100% stdlib, 17/17 operators, 11/11 patterns)
  - Layer 5: Runtime count verification — manifest-based output validation for all 8 test files
  - `run_meta_tests.sh` — master runner for all 5 layers
- **Loop iteration governance enforcement** — `checkLoopIterations()` wired into all 5 loop paths (range-inclusive, range-exclusive, dict, list, while)
- Robustness tests integrated into `run-all-tests.sh` with governance enabled

### Fixed
- **String lexicographic comparison** — `<`, `<=`, `>`, `>=` now compare strings lexicographically instead of converting to float (which always returned false)
- NAAb division always returns float (10/3 = 3.333, not 3) — test expectations corrected
- Functions without explicit return yield last evaluated value, not null — test corrected

### Changed
- Test suite: 340 → 358 tests (0 unexpected failures)
- Stdlib coverage: 75% → 100% (92/92 functions tested, all 7 modules at FULL coverage)
- Robustness suite: 86 → 477 assertions (86 original + 313 exhaustive + 45 mutations + 33 sensitivity)

## [0.5.1] - 2026-03-20

### Fixed
- Float serialization in polyglot bindings — `serializeValueForLanguage()` used `std::to_string()` (6 decimals), now uses `%.15g` matching `Value::toString()` (FIX-1)
- Dict key escaping in polyglot serialization — keys with `"`, `\`, `\n` now properly escaped across all 7 language paths (FIX-2)
- Subscript assignment taint propagation — `dict["k"] = tainted` and `arr[0] = tainted` now mark container as tainted (FIX-3)
- Struct field assignment taint propagation — `obj.field = tainted` now marks struct as tainted (FIX-4)
- Dot-notation method mutation taint — `arr.push(tainted)`, `dict.put("k", tainted)` now propagate taint at 4 code locations (FIX-5)
- Stale mono test assertion — float concatenation test updated from `"3.140000"` to `"3.14"` (FIX-6)
- Documentation: math.pi/math.e gotcha removed (both cases work), float format gotcha updated
- Catch-throw taint restore — exception paths in catch blocks now restore outer taint state (BUG-1)
- ForStmt taint from sources — `for item in env.get("PATH")` now marks loop variable as tainted (BUG-2)
- ReturnStmt taint collapsed to unified `checkRhsTainted()` helper — 33 lines to 3 (BUG-3)
- Parallel polyglot all-language taint — was shell-only, now checks python_exec, go_exec, etc. (BUG-4)
- `checkRhsTainted()` nested CallExpr guard removed + consume-once semantics — `let x = arr[get_tainted()]` detects inner return taint without false-positiving on subsequent literals (BUG-5 + FIX-A)
- Deduplicated polyglot sink-type mapping into `checkPolyglotBoundVarTaint()` helper (FIX-D)

### Changed
- Governance regression suite: 319 → 339 tests (split categories: passes, error behavior, missing executor)
- Mono exhaustive test: 292 → 293 PASS (0 FAIL)
- Test runner categories split: error behavior tests vs missing executor tests vs skip files
- Real taint e2e integration test added (12 scenarios exercising full taint pipeline)

## [0.5.0] - 2026-03-19

### Added
- **Advisory Noise Reduction** — governance output reduced from 100+ lines to 3-5
  - Grouped duplicate call warnings (DX-6): deferred, deduplicated, configurable threshold
  - Grouped polyglot try/catch warnings (DX-7): compact multi-function summary
  - `emitAdvisory()` centralized output with `output.max_advisories` cap (default 15)
  - Suppression summary: "... and N more advisories suppressed"
- **Configurable advisory thresholds** in govern.json:
  - `code_quality.duplicate_calls` — enabled, threshold (default 3), max_entries (default 5)
  - `code_quality.polyglot_try_catch` — enabled, max_entries (default 3)
  - `output.max_advisories` (default 15, 0 = unlimited), `output.advisory_summary`
- **15 DX Governance Improvements** (DX-1 through DX-15):
  - Taint matching changed from substring to prefix (DX-1)
  - All-language polyglot binding taint checks — python_exec, go_exec, etc. (DX-2)
  - MemberExpr sanitizer detection — module.sanitize_foo() clears taint (DX-3)
  - Reserved name warnings — result_, returning_, etc. (DX-4)
  - Unused binding detection within same scope block (DX-5)
  - Duplicate call detection in function bodies (DX-6)
  - Try/catch polyglot advisory warning (DX-7)
  - Scope pattern validation with glob matching (DX-8)
  - Nim JSON error helper for -> JSON blocks (DX-9)
  - Type-safe binding hints for Go/Nim complex types (DX-10)
  - Cross-language hallucination detection — +5 Nim, +3 Go patterns (DX-11)
  - Per-language polyglot_output taint — "polyglot_output:python" syntax (DX-12)
  - JSON roundtrip waste detection (DX-13)
  - Missing executor install guidance per language (DX-14)
  - govern.json schema validation with sanitizer false positive warnings (DX-15)
- **Exhaustive Governance Test Suite** — 197 edge-case tests across 19 files
  - 10 categories: sinks, sources, expressions, async, modules, degraded, recursive, polyglot, interactions, docs
  - Chaos testing: taint washing, silent swallow, direct bypass, concurrent taint, GC stress
- **C++ Native Scanner** (`naab-lang --scan`) — 139 checks across 11 source files
  - 6 categories: redundancy, code_quality, complexity, style, security, language-specific
  - Language modules: Python (14), JavaScript (12), C++ (12), Go (9), Rust (10), NAAb (11)
  - Text + JSON + SARIF output, govern.json configurable
- **GC Cycle Detector Fix** (BUG-10) — env_stack_ tracks all live environments during GC

### Fixed
- Optimization hints verbose 10-line block printed at all enforcement levels — now only for "hard"
- MatchExpr taint propagation — match arms now checked via expressionContainsTaint
- AwaitExpr taint propagation — async return taint chain fixed with lastReturnWasTainted check
- 7 critical review followup fixes: assignment/return/for-stmt await paths, dead code removal
- Polyglot return in functions (BUG-1) — returning_ flag checked in CompoundStmt loop

### Changed
- govern-template.json synced to v4.0 (was v3.0 at root level)
- Governance regression suite: 208 → 339 tests (100% pass rate)

## [0.4.0] - 2026-03-18

### Added
- **Governance v4.0 — Taint Tracking** — variable-level tracking of untrusted data from
  sources (env.get, io.read_line, file.read, polyglot_output) through to sinks (shell_exec,
  file.write, http.*, env.set_var) with configurable enforcement (hard/soft/advisory)
  - Propagation through string concatenation, interpolation, function returns, for-loop iterators
  - Sanitizer functions clear taint (validate_*, sanitize_*, escape_*, int(), float())
  - Name collision fix: taint cleared on variable redeclaration with clean value
  - Thread-safe taint_set_ with mutex for async/parallel execution
  - Taint state saved/restored around module loading to prevent cross-module leaks
- **Cross-Module Input Contracts** — parameter type validation at function call sites
  via govern.json `contracts.functions.*.params` (format: `"name:type"`)
  - `validate_inputs: true` enables input contract checking
  - Checked on both callFunction() and direct call dispatch paths
- **Enhanced Audit Trail** — JSONL logging with tamper-evident hash chains
  - `polyglot_timing`: execution duration per polyglot block
  - `taint_decisions`: every taint mark/clear/block decision logged
  - `contract_checks`: every contract pass/fail logged with details
  - Parallel polyglot blocks now logged in audit trail
- **Dead Conditional Scanner** — regex-based detection for impossible conditions
  - `len(x) < 0`, contradictory comparisons, null access after null check, type impossibilities
  - Data flow analysis: range tracking from math.max/min/clamp/for-loops
- **Configurable Scanner** — govern.json `scanner` section with per-check enable/disable
- **17 Edge Case Bugs Fixed** during exhaustive hardening:
  - Assignment path taint tracking (BUG-A)
  - Expression tree taint propagation for string concat/interpolation (BUG-B/C)
  - Function return taint via lastReturnTainted flag (BUG-D)
  - For-loop iterator taint from tainted iterables (BUG-E)
  - HTTP URL taint sink check / SSRF prevention (BUG-F)
  - env.set_var() taint sink check (BUG-G)
  - Async interpreter governance propagation (BUG-I)
  - Parallel polyglot bound variable taint check (BUG-J)
  - Await resolution return contract check (BUG-K)
  - taint_set_ mutex for thread safety (BUG-N)
  - Module loading taint state isolation (BUG-O)
  - Parallel polyglot audit logging (BUG-P)
  - Direct call path contract audit logging (BUG-Q)

### Changed
- govern.json template updated with v4.0 sections (taint_tracking, enhanced contracts, audit)
- FutureValue now stores func_name for return contract checking at await resolution

### Tests
- 49 governance v4 tests across 12 test files (all passing)
- 208/208 regression suite (100% pass rate)

## [0.3.1] - 2026-03-05

### Added
- **Project Context Awareness** — governance reads project files to supplement `govern.json`
  - Layer 1: LLM instruction files (CLAUDE.md, .cursorrules, copilot-instructions.md, gemini.md)
  - Layer 2: Linter/formatter configs (.editorconfig, .eslintrc, .prettierrc, biome.json)
  - Layer 3: Package manifests (package.json, go.mod, Cargo.toml, pyproject.toml)
  - Opt-in (`"project_context": { "enabled": true }` in govern.json), off by default
  - govern.json always overrides — project context supplements, never conflicts
  - Each layer independently toggleable, dry-run mode, surgical rule suppression
  - Extracted language preferences feed into polyglot optimization scoring
  - Manifest detection is advisory-only — never blocks new languages
  - Full extraction report with source file + line number for every rule
  - 29 edge case tests covering conflicts, priority_source, code block skipping, and more
- Nim, Zig, Julia polyglot executors (3 new languages, 15 total)
- Governance v3.0 — 50+ checks, 13 config sections, per-language rules, custom rules engine
- Polyglot optimization system — 7 detection layers, 205+ patterns, task-language scoring
- Empirical profiling and calibration for governance suggestions
- Cross-language consensus verification

## [0.2.0] - 2025-02-24

### Added
- Pattern matching with `match` expressions
- Async/await support for concurrent function execution
- Dedicated Go executor for polyglot blocks
- REPL tab completion (keywords, stdlib, user symbols, dot notation)
- Lambda expressions with closure capture
- If expressions (`let x = if cond { a } else { b }`)
- Parallel polyglot execution with dependency analysis
- Persistent sub-runtime contexts (`runtime` keyword)
- JSON sovereign pipe (`naab_return()`, `-> JSON` header)
- Native data binding for polyglot blocks (Ruby, PHP, compiled langs)
- Debug module (`debug.inspect`, `debug.type`)
- "Did you mean?" suggestions with fuzzy matching
- Common mistake helper errors (~35 patterns)
- Polyglot error mapping with source location
- Polyglot timeout configuration
- CI workflows: Build & Test, Sanitizers (ASan/UBSan), CodeQL, Supply Chain, Release

### Fixed
- Pipeline operator evaluates right side lazily
- Polyglot `>>` delimiter only closes at line start
- JavaScript type conversion in polyglot blocks
- Python large integer handling
- Python indentation preservation in polyglot blocks
- Thread-safe temp files for C++, Rust, C# executors
- Copy-on-assignment semantics for arrays and dicts
- Break/continue validation outside loops
- Assignment-in-condition detection

## [0.1.0] - 2024-12-01

### Added
- Core language: variables, functions, control flow, loops
- Polyglot block execution (Python, JavaScript, Rust, C++, C#, Shell, Ruby, PHP)
- Standard library: array, string, math, time, env, file, io, json, http, csv, regex, crypto
- REPL with linenoise (arrow keys, history, Ctrl+R search)
- Block registry with SQLite storage
- Error reporting with source locations
