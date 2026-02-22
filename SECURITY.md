# Security Policy

**Last Updated:** 2026-01-30
**Version:** 1.0

## Our Commitment

The NAAb Language project takes security seriously. We are committed to maintaining the security and privacy of our users, and we actively work to identify and fix security vulnerabilities.

## Supported Versions

| Version | Supported          | Security Updates |
| ------- | ------------------ | ---------------- |
| 0.2.x   | :white_check_mark: | Yes (current) |
| < 0.2   | :x:                | No               |

**Note:** Once 1.0 is released, we will maintain security updates for:
- Current major version (1.x): Full support
- Previous major version: Critical security fixes only

## Reporting a Vulnerability

### How to Report

**DO NOT** open a public GitHub issue for security vulnerabilities.

Instead, please report security vulnerabilities via:

1. **GitHub Security Advisory:** https://github.com/b-macker/NAAb/security/advisories/new (preferred)
2. **GitHub Issues (private):** For less critical issues, use the Security tab on the repository

### What to Include

Please include as much information as possible:

- **Description:** Clear description of the vulnerability
- **Impact:** What an attacker could achieve
- **Reproduction:** Step-by-step instructions to reproduce
- **Affected Versions:** Which versions are affected
- **PoC Code:** Proof-of-concept code (if applicable)
- **Suggested Fix:** Proposed fix (if you have one)
- **Your Name:** How you'd like to be credited (optional)

### Response Timeline

We commit to the following response times:

| Priority | Initial Response | Fix Target | Disclosure |
|----------|-----------------|------------|------------|
| **Critical** | 24 hours | 7 days | 30 days |
| **High** | 48 hours | 14 days | 60 days |
| **Medium** | 5 days | 30 days | 90 days |
| **Low** | 10 days | Next release | 120 days |

### What Happens Next

1. **Acknowledgment:** We'll acknowledge receipt within 24-48 hours
2. **Investigation:** We'll investigate and determine severity
3. **Updates:** We'll keep you informed of progress
4. **Fix:** We'll develop and test a fix
5. **Disclosure:** We'll coordinate disclosure with you
6. **Credit:** We'll credit you in release notes (if desired)

## Security Measures

### Current Protections

NAAb has undergone a comprehensive 6-week security hardening sprint and implements:

**Memory Safety:**
- AddressSanitizer (ASan) - Detects memory errors
- UndefinedBehaviorSanitizer (UBSan) - Detects undefined behavior
- MemorySanitizer (MSan) - Detects uninitialized reads
- RAII and smart pointers throughout

**Input Validation:**
- Size limits on all external inputs (10MB file limit)
- Recursion depth limits (10,000 calls)
- Path canonicalization and traversal prevention
- FFI input validation at all boundaries

**Overflow Protection:**
- Arithmetic overflow detection (safe_math)
- Array bounds checking
- Integer overflow protection

**Supply Chain Security:**
- Dependency pinning with lockfiles
- SBOM generation (SPDX, CycloneDX)
- Artifact signing with cosign
- Secret scanning with gitleaks

**Testing:**
- Continuous fuzzing (6 fuzzing targets)
- 28+ comprehensive security tests
- All tests run with sanitizers
- Coverage-guided exploration

**Error Handling:**
- Error message sanitization
- No information leakage
- Production-safe error messages

**Documentation:**
- Comprehensive security documentation
- Threat model analysis
- Incident response playbook

### Security Score

- **Safety Audit:** 90% (A-) - Production ready
- **Coverage:** 144/192 items implemented
- **Blockers:** 0 critical, 0 high priority

See [Chapter 13: Security](docs/book/chapter13.md) for detailed security documentation.

## Known Security Limitations

### By Design

1. **Limited Concurrency:** NAAb has minimal built-in concurrency. Concurrent operations should be handled in polyglot blocks (Python threads, etc.)

2. **Cryptography:** NAAb does not implement custom cryptography. Use polyglot blocks for cryptographic operations.

3. **Sandboxing:** NAAb provides path whitelisting but relies on OS-level sandboxing (containers, etc.) for full isolation.

### Current Gaps (Non-Critical)

1. **Hardware Security:** No hardware fault injection testing (specialized use case)
2. **Formal Verification:** Not formally verified (research project scope)
3. **SLSA Level 3:** Currently Level 2 (hermetic builds pending)

See [Chapter 13: Security](docs/book/chapter13.md) for complete gap analysis.

## Security Best Practices

### For Users

**Running NAAb Code:**
1. **Sandboxing:** Run untrusted code in containers or VMs
2. **File Access:** Use `--allowed-dirs` to restrict file access
3. **Resource Limits:** Use OS resource limits (ulimit, cgroups)
4. **Updates:** Keep NAAb updated with latest security patches

**Writing Secure NAAb Code:**
1. **Input Validation:** Validate all user inputs
2. **Error Handling:** Use try/catch for all external operations
3. **Secrets:** Use environment variables, never hardcode
4. **Polyglot Blocks:** Validate data before passing to polyglot

### For Contributors

**Code Security:**
1. **Follow Guidelines:** See [CONTRIBUTING.md](CONTRIBUTING.md)
2. **Security Review:** All PRs undergo security review
3. **Testing:** Add tests for security-sensitive code
4. **Sanitizers:** Run tests with sanitizers enabled

**Reporting:**
1. **Security Issues:** Use private reporting (see above)
2. **Regular Issues:** Use GitHub issues for non-security bugs
3. **Questions:** Use GitHub Discussions for questions

## Security Contacts

- **GitHub Security:** https://github.com/b-macker/NAAb/security
- **Project Lead:** https://github.com/b-macker

## Security Updates

### Notification Channels

Stay informed about security updates:

1. **GitHub Security Advisories:** https://github.com/b-macker/NAAb/security/advisories
2. **Release Notes:** Check release notes for security fixes
3. **GitHub Watch:** Watch the repository for release notifications

### Update Process

**For Users:**
```bash
# Check current version
naab-lang --version

# Update via package manager (example)
# Debian/Ubuntu
sudo apt update && sudo apt upgrade naab-lang

# Homebrew
brew update && brew upgrade naab-lang

# Or download latest release
wget https://github.com/b-macker/NAAb/releases/latest/naab-lang
```

**Verify Signature:**
```bash
# Download signature and certificate
wget https://github.com/b-macker/NAAb/releases/download/v0.2.0/naab-lang.sig
wget https://github.com/b-macker/NAAb/releases/download/v0.2.0/naab-lang.pem

# Verify with cosign
cosign verify-blob naab-lang \
  --signature naab-lang.sig \
  --certificate naab-lang.pem \
  --certificate-identity-regexp=".*github.com.*" \
  --certificate-oidc-issuer="https://token.actions.githubusercontent.com"
```

## Vulnerability Disclosure Policy

### Coordinated Disclosure

We practice **coordinated disclosure**:

1. **Private Reporting:** Report vulnerabilities privately
2. **Investigation:** We investigate and develop fixes
3. **Coordination:** We coordinate disclosure timing with you
4. **Public Disclosure:** We publish advisory after fix is released
5. **Credit:** We credit researchers (if desired)

### Disclosure Timeline

- **Critical:** 30 days after fix release
- **High:** 60 days after fix release
- **Medium:** 90 days after fix release
- **Low:** Next major release

We may request extended timelines for complex issues.

### Public Disclosure

Public advisories include:

- Vulnerability description
- Affected versions
- Fixed versions
- Mitigation steps
- Credit to researcher
- CVE identifier (if applicable)

## Bug Bounty Program

### Status

**Coming Soon:** We plan to launch a bug bounty program after 1.0 release.

**Scope:** Will cover:
- Memory safety issues
- Input validation bypasses
- Sandbox escapes
- Cryptography misuse
- Information disclosure
- Supply chain vulnerabilities

**Rewards:** To be determined (likely $100-$5000 depending on severity)

**Platform:** Likely HackerOne or BugCrowd

### Current Policy

While no formal bug bounty exists yet, we deeply appreciate security research and will:

1. **Credit** researchers in release notes
2. **Thank** researchers publicly (if desired)
3. **Fast-track** critical fixes
4. **Consider** rewards on case-by-case basis

## Security Hall of Fame

We recognize security researchers who have helped improve NAAb's security:

<!-- Will be populated as researchers report issues -->

**Thank you to all researchers who help keep NAAb secure!**

## Legal

### Safe Harbor

We support security research and will not pursue legal action against researchers who:

1. Make good faith efforts to comply with this policy
2. Report vulnerabilities privately and responsibly
3. Do not exploit vulnerabilities beyond proof-of-concept
4. Do not access or modify user data
5. Do not disrupt NAAb's availability

### Out of Scope

The following are **out of scope**:

- Denial of service attacks
- Social engineering
- Physical attacks
- Attacks requiring physical access
- Issues in third-party dependencies (report to them)
- Issues already disclosed publicly

### Third-Party Dependencies

If you find a vulnerability in one of our dependencies:

1. Report it to the dependency maintainers
2. Also notify us so we can track and update
3. We'll credit you if we release a NAAb security update

## Compliance

NAAb aims to comply with:

- **OWASP Top 10:** All items addressed
- **CWE Top 25:** Most dangerous weaknesses mitigated
- **SLSA Level 2:** Supply chain security (Level 3 in progress)
- **NIST Secure Software Development Framework (SSDF)**

## Questions

For security-related questions:

- **Vulnerability Reports:** https://github.com/b-macker/NAAb/security/advisories/new
- **Non-Security Issues:** GitHub Issues
- **Discussions:** GitHub Discussions

## Resources

- **Security Documentation:** [Chapter 13: Security](docs/book/chapter13.md)
- **Language Reference:** [The NAAb Book](docs/book/)
- **Contributing:** [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)

---

**Thank you for helping keep NAAb and its users safe!**
