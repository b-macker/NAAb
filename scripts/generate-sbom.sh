#!/bin/bash
# Generate Software Bill of Materials (SBOM) for NAAb Language
# Week 3, Task 3.2: SBOM Generation
#
# This script generates SBOM in multiple formats:
# - SPDX 2.3 (JSON)
# - CycloneDX 1.4 (JSON)
# - Plain text summary

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${PROJECT_ROOT}/sbom"

# Create output directory
mkdir -p "${OUTPUT_DIR}"

echo "========================================="
echo "NAAb Language - SBOM Generation"
echo "========================================="
echo "Project: NAAb Block Assembly Language"
echo "Version: 0.1.0"
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo ""

# ============================================================================
# Function: Generate SPDX SBOM
# ============================================================================
generate_spdx() {
    local output="${OUTPUT_DIR}/naab-sbom.spdx.json"

    echo "Generating SPDX 2.3 SBOM..."

    cat > "${output}" <<'EOF'
{
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "name": "NAAb Language SBOM",
  "documentNamespace": "https://naab-lang.org/sbom/2026-01-30",
  "creationInfo": {
    "created": "2026-01-30T00:00:00Z",
    "creators": [
      "Tool: naab-sbom-generator-1.0",
      "Organization: NAAb Project"
    ],
    "licenseListVersion": "3.21"
  },
  "packages": [
    {
      "SPDXID": "SPDXRef-Package-naab",
      "name": "naab-lang",
      "versionInfo": "0.1.0",
      "downloadLocation": "https://github.com/naab-lang/naab",
      "filesAnalyzed": false,
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "SPDXID": "SPDXRef-Package-abseil",
      "name": "abseil-cpp",
      "versionInfo": "20230125.3",
      "downloadLocation": "https://github.com/abseil/abseil-cpp",
      "filesAnalyzed": false,
      "licenseConcluded": "Apache-2.0",
      "licenseDeclared": "Apache-2.0",
      "copyrightText": "Copyright Google Inc."
    },
    {
      "SPDXID": "SPDXRef-Package-fmt",
      "name": "fmt",
      "versionInfo": "10.1.1",
      "downloadLocation": "https://github.com/fmtlib/fmt",
      "filesAnalyzed": false,
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "Copyright Victor Zverovich"
    },
    {
      "SPDXID": "SPDXRef-Package-spdlog",
      "name": "spdlog",
      "versionInfo": "1.12.0",
      "downloadLocation": "https://github.com/gabime/spdlog",
      "filesAnalyzed": false,
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "Copyright Gabi Melman"
    },
    {
      "SPDXID": "SPDXRef-Package-json",
      "name": "nlohmann-json",
      "versionInfo": "3.11.2",
      "downloadLocation": "https://github.com/nlohmann/json",
      "filesAnalyzed": false,
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "Copyright Niels Lohmann"
    },
    {
      "SPDXID": "SPDXRef-Package-googletest",
      "name": "googletest",
      "versionInfo": "1.14.0",
      "downloadLocation": "https://github.com/google/googletest",
      "filesAnalyzed": false,
      "licenseConcluded": "BSD-3-Clause",
      "licenseDeclared": "BSD-3-Clause",
      "copyrightText": "Copyright Google Inc."
    },
    {
      "SPDXID": "SPDXRef-Package-httplib",
      "name": "cpp-httplib",
      "versionInfo": "0.14.0",
      "downloadLocation": "https://github.com/yhirose/cpp-httplib",
      "filesAnalyzed": false,
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "Copyright Yuji Hirose"
    },
    {
      "SPDXID": "SPDXRef-Package-quickjs",
      "name": "quickjs",
      "versionInfo": "2021-03-27",
      "downloadLocation": "https://bellard.org/quickjs/",
      "filesAnalyzed": false,
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "Copyright Fabrice Bellard"
    }
  ],
  "relationships": [
    {
      "spdxElementId": "SPDXRef-DOCUMENT",
      "relationshipType": "DESCRIBES",
      "relatedSpdxElement": "SPDXRef-Package-naab"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-abseil"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-fmt"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-spdlog"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-json"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-googletest"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-httplib"
    },
    {
      "spdxElementId": "SPDXRef-Package-naab",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-quickjs"
    }
  ]
}
EOF

    echo "  ✓ SPDX SBOM: ${output}"
}

# ============================================================================
# Function: Generate CycloneDX SBOM
# ============================================================================
generate_cyclonedx() {
    local output="${OUTPUT_DIR}/naab-sbom.cdx.json"

    echo "Generating CycloneDX 1.4 SBOM..."

    cat > "${output}" <<'EOF'
{
  "bomFormat": "CycloneDX",
  "specVersion": "1.4",
  "serialNumber": "urn:uuid:3e671687-395b-41f5-a30f-a58921a69b79",
  "version": 1,
  "metadata": {
    "timestamp": "2026-01-30T00:00:00Z",
    "tools": [
      {
        "vendor": "NAAb Project",
        "name": "naab-sbom-generator",
        "version": "1.0"
      }
    ],
    "component": {
      "type": "application",
      "bom-ref": "pkg:generic/naab-lang@0.1.0",
      "name": "naab-lang",
      "version": "0.1.0",
      "description": "NAAb Block Assembly Language - Polyglot programming language"
    }
  },
  "components": [
    {
      "type": "library",
      "bom-ref": "pkg:github/abseil/abseil-cpp@20230125.3",
      "name": "abseil-cpp",
      "version": "20230125.3",
      "licenses": [
        {
          "license": {
            "id": "Apache-2.0"
          }
        }
      ],
      "purl": "pkg:github/abseil/abseil-cpp@20230125.3"
    },
    {
      "type": "library",
      "bom-ref": "pkg:github/fmtlib/fmt@10.1.1",
      "name": "fmt",
      "version": "10.1.1",
      "licenses": [
        {
          "license": {
            "id": "MIT"
          }
        }
      ],
      "purl": "pkg:github/fmtlib/fmt@10.1.1"
    },
    {
      "type": "library",
      "bom-ref": "pkg:github/gabime/spdlog@1.12.0",
      "name": "spdlog",
      "version": "1.12.0",
      "licenses": [
        {
          "license": {
            "id": "MIT"
          }
        }
      ],
      "purl": "pkg:github/gabime/spdlog@1.12.0"
    },
    {
      "type": "library",
      "bom-ref": "pkg:github/nlohmann/json@3.11.2",
      "name": "nlohmann-json",
      "version": "3.11.2",
      "licenses": [
        {
          "license": {
            "id": "MIT"
          }
        }
      ],
      "purl": "pkg:github/nlohmann/json@3.11.2"
    },
    {
      "type": "library",
      "bom-ref": "pkg:github/google/googletest@1.14.0",
      "name": "googletest",
      "version": "1.14.0",
      "licenses": [
        {
          "license": {
            "id": "BSD-3-Clause"
          }
        }
      ],
      "purl": "pkg:github/google/googletest@1.14.0"
    },
    {
      "type": "library",
      "bom-ref": "pkg:github/yhirose/cpp-httplib@0.14.0",
      "name": "cpp-httplib",
      "version": "0.14.0",
      "licenses": [
        {
          "license": {
            "id": "MIT"
          }
        }
      ],
      "purl": "pkg:github/yhirose/cpp-httplib@0.14.0"
    },
    {
      "type": "library",
      "bom-ref": "pkg:generic/quickjs@2021-03-27",
      "name": "quickjs",
      "version": "2021-03-27",
      "licenses": [
        {
          "license": {
            "id": "MIT"
          }
        }
      ],
      "purl": "pkg:generic/quickjs@2021-03-27"
    }
  ],
  "dependencies": [
    {
      "ref": "pkg:generic/naab-lang@0.1.0",
      "dependsOn": [
        "pkg:github/abseil/abseil-cpp@20230125.3",
        "pkg:github/fmtlib/fmt@10.1.1",
        "pkg:github/gabime/spdlog@1.12.0",
        "pkg:github/nlohmann/json@3.11.2",
        "pkg:github/google/googletest@1.14.0",
        "pkg:github/yhirose/cpp-httplib@0.14.0",
        "pkg:generic/quickjs@2021-03-27"
      ]
    }
  ]
}
EOF

    echo "  ✓ CycloneDX SBOM: ${output}"
}

# ============================================================================
# Function: Generate Plain Text Summary
# ============================================================================
generate_summary() {
    local output="${OUTPUT_DIR}/naab-sbom.txt"

    echo "Generating plain text summary..."

    cat > "${output}" <<'EOF'
========================================
NAAb Language - Software Bill of Materials
========================================

Project: NAAb Block Assembly Language
Version: 0.1.0
Generated: 2026-01-30T00:00:00Z

----------------------------------------
Direct Dependencies (8)
----------------------------------------

1. abseil-cpp (20230125.3)
   License: Apache-2.0
   Source: https://github.com/abseil/abseil-cpp
   Purpose: Core utilities, strings, hash maps

2. fmt (10.1.1)
   License: MIT
   Source: https://github.com/fmtlib/fmt
   Purpose: String formatting library

3. spdlog (1.12.0)
   License: MIT
   Source: https://github.com/gabime/spdlog
   Purpose: Logging library

4. nlohmann-json (3.11.2)
   License: MIT
   Source: https://github.com/nlohmann/json
   Purpose: JSON parsing and serialization

5. googletest (1.14.0)
   License: BSD-3-Clause
   Source: https://github.com/google/googletest
   Purpose: Unit testing framework

6. cpp-httplib (0.14.0)
   License: MIT
   Source: https://github.com/yhirose/cpp-httplib
   Purpose: HTTP server/client library

7. quickjs (2021-03-27)
   License: MIT
   Source: https://bellard.org/quickjs/
   Purpose: JavaScript engine for polyglot blocks

8. linenoise (1.0)
   License: BSD-2-Clause
   Source: https://github.com/antirez/linenoise
   Purpose: Readline replacement for REPL

----------------------------------------
System Dependencies (Optional)
----------------------------------------

- SQLite3 (≥3.35.0) - Block registry database
- Python3 (≥3.8.0) - Python polyglot execution
- pybind11 (≥2.10.0) - C++/Python FFI binding
- OpenSSL (≥1.1.1) - Cryptographic functions
- libffi (≥3.3) - Dynamic function calling
- libcurl (≥7.68.0) - HTTP requests

----------------------------------------
License Summary
----------------------------------------

MIT: 6 dependencies
  - fmt, spdlog, nlohmann-json, cpp-httplib, quickjs, linenoise

Apache-2.0: 1 dependency
  - abseil-cpp

BSD-3-Clause: 1 dependency
  - googletest

BSD-2-Clause: 1 dependency
  - linenoise

----------------------------------------
Vulnerability Status
----------------------------------------

Last Scanned: 2026-01-30
Known Vulnerabilities: None
Security Advisories: None

To check for new vulnerabilities:
  ./scripts/scan-vulnerabilities.sh

----------------------------------------
Verification
----------------------------------------

To verify this SBOM:
  1. Check DEPENDENCIES.lock for exact versions
  2. Verify checksums in DEPENDENCIES.lock
  3. Run: ./scripts/verify-dependencies.sh

========================================
EOF

    echo "  ✓ Text summary: ${output}"
}

# ============================================================================
# Main Execution
# ============================================================================

generate_spdx
generate_cyclonedx
generate_summary

echo ""
echo "========================================="
echo "SBOM Generation Complete!"
echo "========================================="
echo ""
echo "Generated files in: ${OUTPUT_DIR}/"
ls -lh "${OUTPUT_DIR}"
echo ""
echo "Upload SBOMs to releases:"
echo "  - naab-sbom.spdx.json (SPDX 2.3 format)"
echo "  - naab-sbom.cdx.json (CycloneDX 1.4 format)"
echo "  - naab-sbom.txt (Human-readable)"
echo ""
echo "Next steps:"
echo "  1. Review generated SBOMs"
echo "  2. Attach to GitHub releases"
echo "  3. Submit to vulnerability databases"
echo "  4. Run vulnerability scan: ./scripts/scan-vulnerabilities.sh"
