# Hermetic Builds - SLSA Level 3

**NAAb Language Build Security**
**SLSA Supply Chain Level 3 Compliance**

---

## Table of Contents

1. [Overview](#overview)
2. [SLSA Level 3 Requirements](#slsa-level-3-requirements)
3. [Quick Start](#quick-start)
4. [Build Process](#build-process)
5. [Verification](#verification)
6. [Build Provenance](#build-provenance)
7. [Troubleshooting](#troubleshooting)
8. [Advanced Usage](#advanced-usage)

---

## Overview

### What is a Hermetic Build?

A **hermetic build** is a build process that:
- Has no network access during the build phase
- Uses only pre-declared inputs (vendored dependencies)
- Produces identical outputs given identical inputs (reproducible)
- Generates cryptographic attestations of the build process

### Why Hermetic Builds?

✅ **Supply Chain Security:** Prevents malicious code injection during build
✅ **Reproducibility:** Anyone can verify the build output
✅ **Auditability:** Complete record of what went into the build
✅ **Trust:** Cryptographic proof of build integrity

### SLSA Levels

- **Level 1:** Documentation of build process
- **Level 2:** Version control + Build service
- **Level 3:** ✅ **Hermetic + Reproducible builds** (What we implement)
- **Level 4:** Two-party review + Hermetic + Reproducible

---

## SLSA Level 3 Requirements

### Requirements Met ✅

| Requirement | Status | Implementation |
|-------------|--------|----------------|
| **Source - Version controlled** | ✅ | Git repository |
| **Source - Verified history** | ✅ | Git commit signatures |
| **Source - Retained indefinitely** | ✅ | GitHub/GitLab hosting |
| **Build - Scripted build** | ✅ | CMake + Docker |
| **Build - Build service** | ✅ | Docker containerized |
| **Build - Build as code** | ✅ | Dockerfile.hermetic |
| **Build - Ephemeral environment** | ✅ | Fresh Docker container |
| **Build - Isolated** | ✅ | Container isolation |
| **Provenance - Available** | ✅ | build-provenance.json |
| **Provenance - Authenticated** | ✅ | SHA256/SHA512 checksums |
| **Provenance - Service generated** | ✅ | Automated in Dockerfile |
| **Provenance - Non-falsifiable** | ✅ | Container-generated |
| **Provenance - Dependencies complete** | ✅ | All deps vendored |
| **Common - Security** | ✅ | Hardening flags enabled |
| **Common - Access** | ✅ | Public repository |
| **Common - Superusers** | ✅ | Documented maintainers |

---

## Quick Start

### Prerequisites

- Docker installed and running
- Git (for source control)
- ~2GB free disk space

### Build NAAb Hermetically

```bash
# Clone repository
git clone https://github.com/naab-project/naab.git
cd naab

# Run hermetic build
./scripts/hermetic-build.sh
```

**Output:**
```
========================================
NAAb Hermetic Build (SLSA Level 3)
========================================

✓ Docker found
✓ Docker daemon running
✓ All dependencies vendored
✓ Builder image created
✓ Binaries extracted
✓ Provenance extracted
✓ Build successful and SLSA Level 3 compliant

Artifacts:
  - Binaries: build-hermetic/artifacts/
  - Provenance: build-hermetic/provenance/build-provenance.json
```

### Verify Reproducibility

```bash
# Verify that builds are reproducible
./scripts/verify-reproducibility.sh
```

**Output:**
```
✓ PASS: Build is reproducible

The hermetic build process produces identical outputs
when given identical inputs, confirming reproducibility.
```

---

## Build Process

### Step 1: Environment Setup

The `Dockerfile.hermetic` creates a clean, isolated build environment:

```dockerfile
FROM ubuntu:22.04@sha256:19478ce7fc2ffbce89df29fea5725a8d12e57de52eb9ea570890dc5852aac1ac
```

**Key features:**
- Fixed base image with **digest** (not just tag)
- Specific Ubuntu LTS version
- Minimal base (only what's needed)

### Step 2: Dependency Installation

All build tools installed with **pinned versions**:

```bash
build-essential=12.9ubuntu3
cmake=3.22.1-1ubuntu1.22.04.2
clang-14=1:14.0.0-1ubuntu1.1
```

**Why pin versions?**
- Prevents "version drift" over time
- Ensures same tools are used in every build
- Critical for reproducibility

### Step 3: Source Code Copy

```dockerfile
COPY --chown=builder:builder . ${BUILD_PATH}/
```

All source code and vendored dependencies copied into container.

**No external fetches during build!**

### Step 4: Hermetic Build

```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=${BUILD_PATH}=/build"

cmake --build build --parallel
```

**Hermetic properties:**
- No network access
- Fixed file paths (for reproducibility)
- Deterministic parallelism

### Step 5: Provenance Generation

Automated generation of SLSA provenance:

```json
{
  "buildType": "https://slsa.dev/hermetic-build/v1",
  "metadata": {
    "reproducible": true,
    "hermetic": true
  }
}
```

### Step 6: Artifact Packaging

Final artifacts with checksums:

```
build-hermetic/artifacts/
├── naab-lang              # Binary
├── naab-lang.sha256       # SHA256 checksum
└── naab-lang.sha512       # SHA512 checksum
```

---

## Verification

### Verify Build Provenance

```bash
# View build provenance
cat build-hermetic/provenance/build-provenance.json | jq .
```

**Example output:**
```json
{
  "builder": {
    "id": "https://github.com/naab-project/naab/Dockerfile.hermetic"
  },
  "buildType": "https://slsa.dev/hermetic-build/v1",
  "invocation": {
    "configSource": {
      "uri": "git+https://github.com/naab-project/naab.git",
      "digest": {
        "sha1": "abc123def456..."
      }
    }
  },
  "metadata": {
    "buildFinishedOn": "2026-01-30T20:30:00Z",
    "reproducible": true,
    "hermetic": true
  }
}
```

### Verify Checksums

```bash
# Verify SHA256 checksum
cd build-hermetic/artifacts
sha256sum -c naab-lang.sha256
```

**Output:**
```
naab-lang: OK
```

### Verify Binary Integrity

```bash
# Run the built binary
docker run --rm naab-hermetic-builder:latest --version
```

**Output:**
```
NAAb Language v0.1.0
Build: hermetic (SLSA Level 3)
```

### Verify Reproducibility

Run two independent builds and compare:

```bash
./scripts/verify-reproducibility.sh
```

This script:
1. Builds the source code twice
2. Compares outputs byte-by-byte
3. Verifies bit-for-bit reproducibility

**Expected result:**
```
✓ Binaries are identical (bit-for-bit)
✓ PASS: Build is reproducible
```

---

## Build Provenance

### What is Build Provenance?

Build provenance is a signed attestation that describes:
- What source code was used
- What build system was used
- When the build occurred
- What the resulting artifacts are

### Provenance Format

We use **SLSA Provenance v1** format:

```json
{
  "builder": {
    "id": "URI identifying the builder"
  },
  "buildType": "https://slsa.dev/hermetic-build/v1",
  "invocation": {
    "configSource": {
      "uri": "git+https://...",
      "digest": {"sha1": "..."}
    },
    "environment": {
      "SOURCE_DATE_EPOCH": "1706659200",
      "container_image": "ubuntu:22.04@sha256:..."
    }
  },
  "metadata": {
    "buildFinishedOn": "2026-01-30T20:30:00Z",
    "reproducible": true,
    "hermetic": true
  }
}
```

### Using Provenance

**Verify a downloaded binary:**

```bash
# Download binary and provenance
wget https://releases.naab-lang.org/v0.1.0/naab-lang
wget https://releases.naab-lang.org/v0.1.0/build-provenance.json

# Check that source commit matches expected
jq -r '.invocation.configSource.digest.sha1' build-provenance.json

# Verify it was built hermetically
jq -r '.metadata.hermetic' build-provenance.json
# Output: true
```

---

## Troubleshooting

### Build Fails with "Network Unreachable"

**Problem:** Build tries to fetch external dependencies.

**Solution:** Ensure all dependencies are vendored in `external/`:

```bash
# Check vendored dependencies
ls -la external/
# Should see: abseil-cpp, fmt, googletest, json, spdlog
```

### Builds Are Not Reproducible

**Problem:** Second build produces different output.

**Causes and solutions:**

1. **Timestamps not normalized**
   ```bash
   # Check SOURCE_DATE_EPOCH is set
   grep SOURCE_DATE_EPOCH Dockerfile.hermetic
   ```

2. **Build paths not mapped**
   ```bash
   # Check file-prefix-map is used
   grep "ffile-prefix-map" Dockerfile.hermetic
   ```

3. **Random seed not fixed**
   ```bash
   # Ensure no random seeds are used in build
   grep -r "srand\|random" src/
   ```

### Docker Build Fails

**Problem:** Docker build fails with insufficient memory.

**Solution:** Increase Docker memory limit:

```bash
# Docker Desktop: Preferences → Resources → Memory (increase to 4GB+)
# Linux: Edit /etc/docker/daemon.json
```

### Permission Denied in Container

**Problem:** Build fails with permission errors.

**Solution:** The Dockerfile uses a non-root user. If you need root:

```dockerfile
# Temporarily switch to root if needed
USER root
RUN chmod +x /build/script.sh
USER builder
```

---

## Advanced Usage

### Custom Build Configuration

Override build options via Docker build args:

```bash
docker build \
    -f Dockerfile.hermetic \
    -t naab-custom \
    --build-arg SOURCE_DATE_EPOCH=$(date +%s) \
    --build-arg CMAKE_BUILD_TYPE=Debug \
    .
```

### Multi-Architecture Builds

Build for multiple architectures:

```bash
# Build for ARM64
docker buildx build \
    --platform linux/arm64 \
    -f Dockerfile.hermetic \
    -t naab-hermetic:arm64 \
    .

# Build for AMD64
docker buildx build \
    --platform linux/amd64 \
    -f Dockerfile.hermetic \
    -t naab-hermetic:amd64 \
    .
```

### Continuous Integration

Example GitHub Actions workflow:

```yaml
name: Hermetic Build

on: [push, pull_request]

jobs:
  hermetic-build:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v3

      - name: Build hermetically
        run: ./scripts/hermetic-build.sh

      - name: Verify reproducibility
        run: ./scripts/verify-reproducibility.sh

      - name: Upload artifacts
        uses: actions/upload-artifact@v3
        with:
          name: naab-hermetic
          path: build-hermetic/artifacts/

      - name: Upload provenance
        uses: actions/upload-artifact@v3
        with:
          name: build-provenance
          path: build-hermetic/provenance/
```

### Offline Builds

For completely offline environments:

```bash
# 1. Save Docker image
docker save naab-hermetic-builder:latest -o naab-builder.tar

# 2. Transfer to offline machine
# ... copy naab-builder.tar ...

# 3. Load image
docker load -i naab-builder.tar

# 4. Build (no network needed!)
./scripts/hermetic-build.sh
```

---

## Best Practices

### For Developers

✅ **DO:**
- Vendor all dependencies in `external/`
- Use fixed versions for all tools
- Normalize timestamps with `SOURCE_DATE_EPOCH`
- Test reproducibility regularly
- Document any non-determinism

❌ **DON'T:**
- Fetch dependencies during build
- Use floating version tags
- Include timestamps in binaries
- Use random number generators
- Rely on external services

### For Release Managers

✅ **DO:**
- Always build from tagged releases
- Verify provenance before releasing
- Sign release artifacts
- Publish provenance with binaries
- Test reproducibility on clean machines

❌ **DON'T:**
- Build from uncommitted changes
- Skip provenance generation
- Release unsigned binaries
- Trust builds without verification

---

## Security Considerations

### Threat Model

**Threats mitigated by hermetic builds:**
- ✅ Malicious dependency injection
- ✅ Build-time backdoor insertion
- ✅ Compromised build servers
- ✅ Supply chain attacks

**Threats NOT mitigated:**
- ❌ Malicious source code
- ❌ Compromised developer machines
- ❌ Social engineering attacks

### Security Recommendations

1. **Verify base image digest**
   ```bash
   # Always use digest, not tag
   FROM ubuntu:22.04@sha256:...
   ```

2. **Pin all dependencies**
   ```bash
   # Include version numbers
   apt-get install cmake=3.22.1-1ubuntu1.22.04.2
   ```

3. **Minimize attack surface**
   ```dockerfile
   # Multi-stage builds - final image has minimal runtime deps
   FROM ubuntu:22.04 AS builder
   # ... build ...
   FROM ubuntu:22.04
   COPY --from=builder /opt/naab /opt/naab
   ```

4. **Regular updates**
   ```bash
   # Update base image digest monthly
   docker pull ubuntu:22.04
   docker inspect ubuntu:22.04 | grep RepoDigests
   ```

---

## References

### SLSA Documentation
- [SLSA Levels](https://slsa.dev/spec/v1.0/levels)
- [SLSA Requirements](https://slsa.dev/spec/v1.0/requirements)
- [Build Provenance](https://slsa.dev/provenance/v1)

### Reproducible Builds
- [Reproducible Builds Project](https://reproducible-builds.org/)
- [SOURCE_DATE_EPOCH](https://reproducible-builds.org/docs/source-date-epoch/)
- [Debian Reproducibility](https://wiki.debian.org/ReproducibleBuilds)

### Docker Best Practices
- [Docker Security](https://docs.docker.com/engine/security/)
- [Multi-stage Builds](https://docs.docker.com/build/building/multi-stage/)
- [BuildKit](https://docs.docker.com/build/buildkit/)

---

## Changelog

### Version 1.0 (2026-01-30)

**Initial SLSA Level 3 Implementation:**
- ✅ Hermetic Docker builds
- ✅ Reproducibility verification
- ✅ Build provenance generation
- ✅ Automated build scripts
- ✅ Complete documentation

**Security improvements:**
- Vendored all dependencies
- Pinned package versions
- Fixed base image digest
- Normalized timestamps
- Mapped build paths

---

## Support

### Getting Help

- **Documentation:** This file + `docs/SECURITY.md`
- **Issues:** https://github.com/naab-project/naab/issues
- **Security:** security@naab-lang.org

### Contributing

Improvements to the hermetic build process are welcome!

Please ensure:
- Reproducibility is maintained
- SLSA Level 3 compliance is preserved
- Changes are documented
- Tests pass

---

**Document Version:** 1.0
**Last Updated:** 2026-01-30
**SLSA Level:** 3 (Hermetic + Reproducible)
**Status:** Production Ready ✅
