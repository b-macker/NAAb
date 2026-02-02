#!/bin/bash
# Sign NAAb Language Release Artifacts
# Week 3, Task 3.3: Artifact Signing
#
# This script signs release artifacts using cosign (keyless signing with OIDC)
# or GPG (traditional signing with keys)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Configuration
SIGN_METHOD="${SIGN_METHOD:-cosign}"  # cosign or gpg
ARTIFACT_DIR="${PROJECT_ROOT}/build/release"

echo "========================================="
echo "NAAb Language - Artifact Signing"
echo "========================================="
echo "Method: ${SIGN_METHOD}"
echo "Artifact directory: ${ARTIFACT_DIR}"
echo ""

# ============================================================================
# Function: Check if cosign is installed
# ============================================================================
check_cosign() {
    if ! command -v cosign &> /dev/null; then
        echo "Error: cosign not found"
        echo ""
        echo "Install cosign:"
        echo "  # Linux"
        echo "  curl -sSfL https://github.com/sigstore/cosign/releases/latest/download/cosign-linux-amd64 -o /usr/local/bin/cosign"
        echo "  chmod +x /usr/local/bin/cosign"
        echo ""
        echo "  # macOS"
        echo "  brew install cosign"
        echo ""
        return 1
    fi
    echo "✓ cosign found: $(cosign version 2>&1 | head -1)"
}

# ============================================================================
# Function: Check if GPG is installed
# ============================================================================
check_gpg() {
    if ! command -v gpg &> /dev/null; then
        echo "Error: gpg not found"
        echo ""
        echo "Install GPG:"
        echo "  sudo apt-get install gnupg"
        echo ""
        return 1
    fi
    echo "✓ gpg found: $(gpg --version | head -1)"
}

# ============================================================================
# Function: Sign with cosign (keyless OIDC signing)
# ============================================================================
sign_with_cosign() {
    local artifact="$1"
    local signature="${artifact}.sig"
    local certificate="${artifact}.pem"

    echo "Signing ${artifact} with cosign..."

    # Sign artifact (keyless signing with OIDC)
    # User will be prompted to authenticate via browser
    cosign sign-blob "${artifact}" \
        --output-signature="${signature}" \
        --output-certificate="${certificate}" \
        --yes  # Skip confirmation

    echo "  ✓ Signature: ${signature}"
    echo "  ✓ Certificate: ${certificate}"

    # Verify signature
    echo "  Verifying signature..."
    cosign verify-blob "${artifact}" \
        --signature="${signature}" \
        --certificate="${certificate}" \
        --certificate-identity-regexp=".*" \
        --certificate-oidc-issuer-regexp=".*"

    echo "  ✓ Signature verified"
}

# ============================================================================
# Function: Sign with GPG (traditional key-based signing)
# ============================================================================
sign_with_gpg() {
    local artifact="$1"
    local signature="${artifact}.asc"

    echo "Signing ${artifact} with GPG..."

    # Create detached ASCII-armored signature
    gpg --detach-sign --armor --output "${signature}" "${artifact}"

    echo "  ✓ Signature: ${signature}"

    # Verify signature
    echo "  Verifying signature..."
    gpg --verify "${signature}" "${artifact}"

    echo "  ✓ Signature verified"
}

# ============================================================================
# Function: Generate checksums
# ============================================================================
generate_checksums() {
    local artifact="$1"
    local sha256_file="${artifact}.sha256"
    local sha512_file="${artifact}.sha512"

    echo "Generating checksums for ${artifact}..."

    # SHA-256
    if command -v sha256sum &> /dev/null; then
        sha256sum "${artifact}" > "${sha256_file}"
        echo "  ✓ SHA-256: ${sha256_file}"
    fi

    # SHA-512
    if command -v sha512sum &> /dev/null; then
        sha512sum "${artifact}" > "${sha512_file}"
        echo "  ✓ SHA-512: ${sha512_file}"
    fi
}

# ============================================================================
# Function: Sign single artifact
# ============================================================================
sign_artifact() {
    local artifact="$1"

    if [ ! -f "${artifact}" ]; then
        echo "Error: Artifact not found: ${artifact}"
        return 1
    fi

    echo "========================================="
    echo "Artifact: $(basename ${artifact})"
    echo "Size: $(du -h ${artifact} | cut -f1)"
    echo "========================================="

    # Generate checksums first
    generate_checksums "${artifact}"

    # Sign based on method
    if [ "${SIGN_METHOD}" = "cosign" ]; then
        sign_with_cosign "${artifact}"
    elif [ "${SIGN_METHOD}" = "gpg" ]; then
        sign_with_gpg "${artifact}"
    else
        echo "Error: Unknown signing method: ${SIGN_METHOD}"
        echo "Use: SIGN_METHOD=cosign or SIGN_METHOD=gpg"
        return 1
    fi

    echo ""
}

# ============================================================================
# Main Execution
# ============================================================================

# Check prerequisites
if [ "${SIGN_METHOD}" = "cosign" ]; then
    check_cosign || exit 1
elif [ "${SIGN_METHOD}" = "gpg" ]; then
    check_gpg || exit 1
fi

echo ""

# Sign artifacts
if [ $# -eq 0 ]; then
    echo "Usage: $0 <artifact1> [artifact2] ..."
    echo ""
    echo "Examples:"
    echo "  # Sign single binary"
    echo "  $0 build/naab-lang"
    echo ""
    echo "  # Sign multiple files"
    echo "  $0 build/naab-lang build/naab-repl"
    echo ""
    echo "  # Sign with GPG instead of cosign"
    echo "  SIGN_METHOD=gpg $0 build/naab-lang"
    echo ""
    echo "  # Sign all release artifacts"
    echo "  $0 build/release/*"
    exit 1
fi

# Sign each artifact
for artifact in "$@"; do
    sign_artifact "${artifact}"
done

echo "========================================="
echo "Signing Complete!"
echo "========================================="
echo ""
echo "Generated files:"
find "$(dirname $1)" -name "*.sig" -o -name "*.pem" -o -name "*.asc" -o -name "*.sha256" -o -name "*.sha512" | head -20
echo ""
echo "Verification:"
if [ "${SIGN_METHOD}" = "cosign" ]; then
    echo "  cosign verify-blob <artifact> --signature=<artifact>.sig --certificate=<artifact>.pem \\"
    echo "    --certificate-identity-regexp='.*' --certificate-oidc-issuer-regexp='.*'"
elif [ "${SIGN_METHOD}" = "gpg" ]; then
    echo "  gpg --verify <artifact>.asc <artifact>"
fi
echo ""
echo "Checksums:"
echo "  sha256sum -c <artifact>.sha256"
echo "  sha512sum -c <artifact>.sha512"
echo ""
echo "Upload to release:"
echo "  - Artifact: <artifact>"
echo "  - Signature: <artifact>.sig (cosign) or <artifact>.asc (GPG)"
echo "  - Certificate: <artifact>.pem (cosign only)"
echo "  - Checksums: <artifact>.sha256, <artifact>.sha512"
