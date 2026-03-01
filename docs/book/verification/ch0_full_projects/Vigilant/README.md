# 🛡️ Project VIGILANT v7.0: Industrial Sovereign Fabric

**Version:** 7.0 "Industrial"  
**Architecture:** Sovereign Polyglot Orchestration  
**Runtime:** NAAb Native  

## 📜 Overview
Project VIGILANT v7.0 is a **Sovereign Security Appliance** designed to enforce enterprise PII (Personally Identifiable Information) policies with mathematical certainty. It moves beyond traditional proxy architectures by establishing a **Absolute Logic Sovereignty** model where all decision-making is centralized in a memory-locked NAAb brain, while performance-critical tasks are delegated to hardware-isolated polyglot "vessels."

## 🏗️ The Sovereign Architecture
VIGILANT decomposes the security stack into three distinct layers, governed by the **NAAb Master Orchestrator**:

1.  **The Brain (NAAb/Python)**: 
    *   **Role**: Absolute Arbitrator.
    *   **Logic**: Implements Strict Schema Validation (Anti-Smuggling), Probabilistic Risk Scoring, and Surgical Redaction.
    *   **Hardening**: Executes in a memory-locked processing space (`mlockall`) to prevent RAM-to-disk leakage.
2.  **The Pipe (Go)**:
    *   **Role**: Dumb Protocol Adapter.
    *   **Logic**: A minimalist, high-concurrency bridge that converts HTTP/TLS traffic into a raw UDS (Unix Domain Socket) stream for the Brain.
    *   **Efficiency**: Uses Round-Robin balancing and persistent connection pooling.
3.  **The Muscle (Rust)**:
    *   **Role**: Deterministic Scanner.
    *   **Logic**: Constant-time regex and pattern matching to prevent timing side-channel attacks.
    *   **Isolation**: Launched in a private network namespace (`unshare -n`) with no egress.

## 🛡️ Key Security Features
*   **Zero-Trust Anti-Smuggling**: Strict schema enforcement rejects any JSON payload containing unknown keys or type mismatches before processing begins.
*   **Hardware Isolation**: Workers are pinned to physical CPU cores via `taskset` to eliminate CPU cache side-channels.
*   **Smart Cache Synthesis**: SHA-256 source hashing allows the system to verify existing binaries, enabling millisecond "warm starts" while maintaining the integrity of the self-synthesizing model.
*   **Forensic Shredding**: Temporary source code generated during the synthesis phase is overwritten with random data (3-pass) before being unlinked.
*   **Immutable Audit Provenance**: All security events are logged into a hash-chained, cryptographically signed audit log.

## 🚀 Usage

### Requirements
*   Linux Kernel with `unshare` and `taskset` support (Android/Termux compatible).
*   `rustc`, `go`, and `python3` toolchains.
*   NAAb Runtime.

### Launching the Appliance
The master orchestrator handles synthesis, hardening, and process lifecycle:
```bash
naab-lang run verification/ch0_full_projects/Vigilant/main.naab
```

### Running the Industrial Regression Suite
Verify the fabric's resilience against adversarial PII exfiltration and schema smuggling:
```bash
naab-lang run verification/ch0_full_projects/Vigilant/verify_vigilant_v7.naab
```

## 📊 Technical Audit
| Component | Technology | Isolation Tier |
| :--- | :--- | :--- |
| **Orchestrator** | NAAb | Master Sovereign |
| **Gateway** | Go | User-Space Bridge |
| **Decision Engine** | Python/NAAb | Memory-Locked (`mlockall`) |
| **Scanner** | Rust | Private Namespace (`unshare`) |
| **Persistence** | Hash-Chained Logs | Cryptographic Integrity |

---
**VIGILANT v7.0** represents the pinnacle of NAAb-native engineering—a system that doesn't just process data, but **commands** the environment in which data is processed. 🛡️💎🌟
