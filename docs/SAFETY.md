Safety checksheet


Memory safety and data integrity

Enforce bounds validation for all buffer accesses.

Prohibit unchecked pointer arithmetic in production code.

Disallow raw memory copies without explicit size validation.

Prevent implicit integer widening or narrowing without explicit casts.

Prevent signed/unsigned confusion in arithmetic and comparisons.

Detect and guard against arithmetic overflow and underflow.

Prevent reads of uninitialized memory.

Prevent double free, use‑after‑free, and stale references.

Prevent aliasing violations that break invariants.

Disallow stack allocations sized by untrusted input.

Enforce recursion depth limits where applicable.

Handle zero‑length allocations safely.

Detect integer wraparound in time and counter calculations.

Disallow pointer comparisons across unrelated objects.

Ensure allocators zero memory when required by policy.

Prevent memory reuse patterns that leak prior contents.

Use memory fences where concurrency requires ordering.

Enforce bounds‑checked containers and slices by default.

Provide mandatory safe arithmetic APIs for security code.

Automatically zeroize sensitive data on free or scope exit.

Enable compiler‑enforced lifetime tracking where available.

Run memory and UB sanitizers in CI (ASan, UBSan, MSan).

Use memory tagging or hardware safety features where available.

Ensure deterministic destructors or RAII for resource cleanup.

Test behavior under extreme resource exhaustion (fds, threads, memory).

Type safety and data validation

Disallow unchecked type casts in security‑sensitive code.

Avoid dynamic type assertions without safe fallback handling.

Prevent implicit coercions from untrusted input.

Disallow reflection‑based access to private members without audit.

Validate types during serialization and deserialization strictly.

Prevent type confusion via polymorphic or serialized inputs.

Disallow unsafe downcasting in plugin or extension systems.

Reject deserialization of attacker‑crafted cyclic or malicious graphs.

Enforce strict schema validation for all serialized formats.

Use compile‑time type narrowing and exhaustive checks.

Disallow runtime mutation of types or type metadata for shared data.

Enforce immutability for shared or concurrently accessed data.

Input handling and parsing

Disallow unchecked string operations and implicit truncation.

Enforce input size caps for all external inputs.

Use canonicalization before validation for filenames, URLs, and identifiers.

Use strict parser combinators or generated parsers instead of ad‑hoc parsing.

Enforce regex timeouts and sandboxing for complex patterns.

Prevent parser states reachable via malformed input.

Handle Unicode normalization and homoglyph issues explicitly.

Reject overlong UTF‑8 sequences and mixed newline conventions.

Handle BOMs and embedded null bytes safely.

Prevent multi‑byte boundary split vulnerabilities.

Maintain curated fuzz seed corpora for each parser.

Use grammar‑based fuzzing for structured inputs.

Concurrency and synchronization

Disallow shared mutable state without explicit synchronization.

Prevent data races; run static race detectors in CI.

Avoid blocking operations inside critical sections.

Enforce lock ordering policies to prevent deadlocks.

Detect and prevent priority inversion where relevant.

Test for atomicity violations under high contention.

Detect and mitigate TOCTOU race windows.

Test for deadlocks triggered by rare interleavings.

Prevent thread‑local storage leaks across thread pools.

Prefer immutable data structures for concurrent sharing.

Use structured concurrency primitives and scoped threads.

Use lock‑free algorithms only with proven correctness and tests.

Run concurrency fuzzing and stress tests in CI.

Error handling and recovery

Do not swallow exceptions or ignore error codes.

Disallow fallback to insecure defaults on error.

Prevent logging of sensitive data in error paths.

Ensure error paths always perform cleanup.

Prevent exceptions thrown during cleanup from causing leaks.

Avoid recursive error handling loops.

Ensure error messages do not leak internal state or secrets.

Enforce mandatory error propagation and structured error types.

Centralize error handling policies and document allowed abort contexts.

Require postmortem reproducibility steps in incident tickets.

Cryptographic safety

Prohibit custom cryptography; use vetted libraries.

Use secure randomness (hardware‑backed RNG where available).

Disallow deprecated algorithms and weak key sizes.

Prohibit hardcoded keys and secrets in source or build logs.

Prevent nonce reuse and IV collisions; enforce unique nonces.

Use constant‑time comparisons for sensitive comparisons.

Zeroize key material and secrets in memory after use.

Provide constant‑time primitives for crypto operations.

Enforce crypto policy and algorithm allowlists in CI.

Validate cryptographic implementations with test vectors.

Compiler, interpreter, and runtime safety

Do not rely on undefined behavior; treat UB as a security risk.

Avoid relying on compiler‑specific quirks or undocumented behavior.

Prevent JIT optimizations from bypassing security checks.

Disallow unsafe runtime reflection without audit.

Build with multiple compilers and compare behavior to detect miscompilations.

Rebuild with different optimization levels and verify semantics.

Run compiler fuzzing or differential testing on toolchain inputs where feasible.

Treat compiler and linker as part of the trusted computing base and verify provenance.

Enable UB sanitizers and run them in CI for security modules.

Use deterministic and reproducible builds; verify reproducibility.

Detect and fail on unexpected toolchain environment variables in CI.

Verify interpreter safety modes and enable verified interpreter modes where available.

Detect garbage collector race conditions and test GC interactions.

Verify tail‑call elimination does not break security assumptions.

FFI, ABI, and boundary safety

Minimize FFI surface area; expose only necessary functions.

Validate and copy inputs at FFI boundaries into owned memory.

Define and document explicit ownership transfer semantics across FFI.

Use safe marshaling layers or generated wrappers for FFI.

Audit and test every unsafe block and FFI wrapper with malformed inputs.

Enforce ABI compatibility tests in CI for public/native interfaces.

Detect and reject mismatched calling conventions between modules.

Enforce explicit endianness handling for cross‑platform data.

Reject implicit string terminators; require length‑prefixed messages.

Detect and handle ABI alignment and padding differences.

Prevent variadic function misuse across boundaries.

Prevent foreign code from throwing exceptions across language boundaries.

Instrument and log pointer provenance for FFI calls during tests.

Boundary fuzzing for all FFI and serialization interfaces.

Supply chain and build integrity

Pin all dependencies and lock transitive versions.

Generate and publish SBOMs for every release artifact.

Sign artifacts and enforce signature verification at install/load time.

Use hermetic builds and sandbox build steps.

Disallow build scripts that execute remote code without review.

Enforce dependency allowlists and block unvetted packages.

Verify transitive dependency signatures and reject unsigned transitive artifacts.

Detect and reject dependency version ranges that allow silent upgrades.

Compare SBOMs across builds to detect injected or removed components.

Audit and sandbox build‑time plugins and CI runners as part of the TCB.

Monitor dependency advisories and automate vetted patch PRs.

Enforce reproducible build verification in CI.

Automate SBOM diff alerts for unexpected component changes.

Require SLSA or equivalent provenance verification for critical releases.

Maintain a bug bounty or coordinated disclosure program for codebase reporting.

Logic, design, and semantic safety

Do not trust client‑side validation; enforce server‑side checks.

Avoid implicit assumptions about input ordering or timing.

Prevent business‑logic bypasses and privilege escalation paths.

Model and test rare state transitions and multi‑step workflows.

Detect race‑dependent logic and time‑dependent logic vulnerabilities.

Protect against floating‑point precision attacks where relevant.

Encode formal invariants and document module assumptions.

Use property‑based testing and model checking for critical flows.

Implement explicit state machines for complex workflows.

Require threat modeling and documented assumptions before design sign‑off.

File, process, and OS interaction

Canonicalize paths and prevent path traversal.

Use safe temp file APIs and avoid predictable temp names.

Prevent shell command injection; avoid shelling out with untrusted input.

Sanitize environment variables and avoid unsafe reliance on them.

Handle Unicode path tricks and normalization for filesystem checks.

Prevent symlink and hardlink race attacks.

Prevent file descriptor reuse vulnerabilities.

Enforce strict syscall allowlists (seccomp/BPF) for native processes.

Apply resource quotas and fail closed on resource exhaustion.

Verify filesystem atomicity assumptions on target filesystems.

Validate behavior when system entropy is low or unavailable.

Testing, fuzzing, and verification

Unit tests for all input boundaries and error paths.

Continuous fuzzing for all parsers and external input handlers.

Coverage‑guided fuzzing for critical code paths.

Maintain and curate diverse fuzz seed corpora; minimize and preserve regression seeds.

Differential fuzzing across compilers and runtimes.

Grammar‑based and structured fuzzing for complex formats.

Concurrency fuzzing and stress testing for race conditions.

Fault‑injection testing and chaos tests including security failure modes.

Mutation testing to validate test suite effectiveness.

Symbolic execution for deep edge‑case exploration where feasible.

Formal verification for bootloaders, crypto, and safety‑critical modules.

Mandatory sanitizer builds in CI and no permanent suppressions without review.

Automate crash triage: dedupe, minimize, and link crashes to commits.

Regression tests for all past vulnerabilities and CVEs.

Observability, telemetry, and incident readiness

Emit structured, tamper‑evident audit logs for security events.

Ensure runtime telemetry for sanitizer‑style crashes and unusual exception rates.

Record provenance metadata (commit, toolchain, SBOM) with every deployed binary.

Implement forensic readiness: secure log retention and documented triage playbooks.

Automate crash deduplication and link to source for rapid triage.

Canary deployments with enhanced telemetry and automatic rollback.

Maintain documented incident playbooks including artifact revocation and rollback.

Automate detection of sanitizer‑only failures surfaced in CI and production.

Secrets, keys, and runtime protection

Prohibit secrets in source, CI logs, or build artifacts.

Use CI secrets vaulting and automated secret rotation for build agents.

Enforce secret scanning in PRs and artifact repositories with blocking rules.

Use TPM/HSM for key storage and signing where available.

Enforce ephemeral credentials and least privilege for service accounts.

Automatic key zeroization and secure memory handling for secrets.

Enforce export control and cryptography policy compliance in builds.

Hardware, microarchitectural, and platform mitigations

Enable and test microarchitectural mitigations (Spectre/Meltdown) where required.

Implement cache‑timing and branch‑prediction side‑channel resistant code for sensitive paths.

Use hardware root of trust and secure boot verification for runtime images.

Enable memory tagging, SMEP/SMAP, and ECC detection where supported.

Detect and mitigate CPU microcode or speculative execution anomalies at startup.

Include runtime checksums for critical in‑memory tables to detect silent corruption.

Design and test for partial hardware failures and intermittent I/O in CI.

Fail safe on detected CPU feature mismatches between build and runtime hosts.

Use remote attestation and enclave/TEE verification for sensitive modules where applicable.

Policy, governance, and process controls

Map each checklist item to CI gates and enforcement actions.

Classify items by risk level (blocker, high, medium, low) and prioritize remediation.

Automate evidence collection and attach provenance to release artifacts.

Require documented justification and peer review for every unsafe/native/FFI change.

Mandate threat modeling and security requirements before design sign‑off.

Require security review checklists in PR templates and block merges until checks pass.

Schedule regular red‑team and supply‑chain exercises targeting CI and toolchain.

Maintain a bug bounty or coordinated disclosure program and integrate reports into triage.

Train developers on constant‑time coding, side‑channel risks, and secure FFI patterns.

Maintain minimal trusted computing base for build infrastructure and use ephemeral runners.

Periodically run meta‑tests that attempt to subvert the checklist and CI controls.

Continuously reassess and update the checklist after toolchain, dependency, or hardware changes.

Small but critical nitcases and edge checks

Reject or canonicalize locale‑dependent string comparisons used in security checks.

Treat time sources as untrusted; validate and bound clock skew assumptions.

Avoid relying on filesystem atomicity unless verified on the target FS.

Verify behavior under extreme resource exhaustion and low entropy.

Fail closed if runtime memory limits or ulimits are unexpectedly high or low.

Detect and refuse to run with debug or unsafe environment flags in production.

Detect and reject dependency cache poisoning and build cache tampering.

Enforce automated license and export‑control compliance checks for dependencies.

Require periodic third‑party code audits and signed attestations from critical maintainers.

Maintain SLA and automated rollback for emergency revocation of compromised artifacts.

How to use this checksheet

Audit: mark each line implemented/missing/partial for each module.

Automate: convert high‑risk items into CI gates and automated checks.

Prioritize: remediate blocker and high items first (FFI validation, sanitizers, SBOMs, signing).

Operate: attach provenance metadata to every build and enforce signed, reproducible releases.

Iterate: schedule quarterly reviews and run meta‑tests to validate the checklist itself.


