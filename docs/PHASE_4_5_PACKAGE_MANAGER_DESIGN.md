# Phase 4.5: Package Manager (naab-pkg) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** HIGH - Requires dependency resolution, registry, versioning
**Estimated Effort:** 5-6 weeks implementation
**Priority:** MEDIUM-HIGH - Essential for ecosystem growth

This document outlines the design for `naab-pkg`, NAAb's package manager for managing dependencies, publishing packages, and building a package ecosystem.

---

## Current Problem

**No Package Management:**
- Cannot easily share code between projects
- No standard way to distribute libraries
- Must manually copy files or use git submodules
- No versioning or dependency resolution
- Difficult to build ecosystem

**Impact:** Limited code reuse, difficult collaboration, slow ecosystem growth.

---

## Goals

### Primary Goals

1. **Package Manifest** - naab.json for project metadata
2. **Dependency Installation** - Install packages with versions
3. **Dependency Resolution** - Resolve transitive dependencies
4. **Package Publishing** - Publish packages to registry
5. **Module System** - Import packages in code

### Secondary Goals

6. **Lockfile** - Reproducible builds (naab.lock)
7. **Local/Remote** - Support local development + remote packages
8. **Semantic Versioning** - Standard versioning scheme
9. **Private Packages** - Private registry support

---

## Architecture

### Components

```
┌──────────────────────────────────────┐
│        User Project                   │
│                                       │
│  ├── naab.json       (manifest)      │
│  ├── naab.lock       (lockfile)      │
│  ├── naab_modules/   (dependencies)  │
│  └── src/main.naab                   │
└────────────┬─────────────────────────┘
             │
             │ naab-pkg install
             ▼
┌────────────────────────────────────┐
│    Package Registry                │
│    (Central or Git-based)          │
│                                    │
│  - Package metadata                │
│  - Package tarballs                │
│  - Version history                 │
└────────────────────────────────────┘
```

### Project Structure

```
tools/naab-pkg/
├── main.cpp                      # CLI entry point
├── package_manager.h/cpp        # Core package manager
├── manifest.h/cpp               # naab.json parsing
├── lockfile.h/cpp               # naab.lock handling
├── dependency_resolver.h/cpp    # Dependency resolution
├── registry_client.h/cpp        # Registry API client
├── installer.h/cpp              # Package installation
└── publisher.h/cpp              # Package publishing
```

---

## Package Manifest (naab.json)

### Format

```json
{
  "name": "my-package",
  "version": "1.0.0",
  "description": "A sample NAAb package",
  "author": "Alice <alice@example.com>",
  "license": "MIT",
  "main": "src/main.naab",

  "dependencies": {
    "http": "^2.0.0",
    "json": "^1.5.0",
    "string-utils": "~3.2.1"
  },

  "devDependencies": {
    "test-framework": "^1.0.0",
    "linter": "^2.1.0"
  },

  "scripts": {
    "test": "naab-test tests/",
    "build": "naab-build src/",
    "lint": "naab-lint src/"
  },

  "repository": {
    "type": "git",
    "url": "https://github.com/user/my-package.git"
  },

  "keywords": ["http", "api", "client"],
  "homepage": "https://github.com/user/my-package"
}
```

### Version Specifiers

**Exact Version:**
```json
"http": "2.0.0"
```

**Caret (^) - Compatible:** Minor/patch updates allowed
```json
"http": "^2.0.0"  // Allows 2.0.1, 2.1.0, but not 3.0.0
```

**Tilde (~) - Patch:** Only patch updates allowed
```json
"http": "~2.0.0"  // Allows 2.0.1, 2.0.2, but not 2.1.0
```

**Range:**
```json
"http": ">=2.0.0 <3.0.0"
```

**Latest:**
```json
"http": "*"  // Latest version (not recommended)
```

---

## Lockfile (naab.lock)

### Purpose

- Ensure reproducible builds
- Lock exact versions of all dependencies (including transitive)
- Commit to version control

### Format

```json
{
  "lockfileVersion": 1,
  "dependencies": {
    "http": {
      "version": "2.0.3",
      "resolved": "https://registry.naab.dev/http/-/http-2.0.3.tgz",
      "integrity": "sha512-abc123...",
      "dependencies": {
        "net": {
          "version": "1.2.0",
          "resolved": "https://registry.naab.dev/net/-/net-1.2.0.tgz",
          "integrity": "sha512-def456..."
        }
      }
    },
    "json": {
      "version": "1.5.2",
      "resolved": "https://registry.naab.dev/json/-/json-1.5.2.tgz",
      "integrity": "sha512-ghi789..."
    }
  }
}
```

---

## Commands

### naab-pkg init

**Create new package:**

```bash
naab-pkg init
```

**Interactive prompts:**
```
Package name: my-package
Version: (1.0.0)
Description: My NAAb package
Entry point: (src/main.naab)
Author: Alice <alice@example.com>
License: (MIT)
```

**Creates:** `naab.json` with defaults

**Implementation:**

```cpp
void PackageManager::init() {
    if (fs::exists("naab.json")) {
        throw Error("naab.json already exists");
    }

    // Interactive prompts
    std::string name = prompt("Package name");
    std::string version = prompt("Version", "1.0.0");
    std::string description = prompt("Description");
    std::string author = prompt("Author");
    std::string license = prompt("License", "MIT");

    // Create manifest
    json manifest = {
        {"name", name},
        {"version", version},
        {"description", description},
        {"author", author},
        {"license", license},
        {"main", "src/main.naab"},
        {"dependencies", json::object()},
        {"devDependencies", json::object()}
    };

    // Write to file
    std::ofstream file("naab.json");
    file << manifest.dump(2);
}
```

---

### naab-pkg install

**Install all dependencies:**

```bash
naab-pkg install
```

**Install specific package:**

```bash
naab-pkg install http
naab-pkg install http@2.0.0
naab-pkg install http@^2.0.0
```

**Install and save to dependencies:**

```bash
naab-pkg install http --save
naab-pkg install test-framework --save-dev
```

**Implementation:**

```cpp
void PackageManager::install(const std::string& package_name, const std::string& version_spec) {
    // 1. Load manifest
    Manifest manifest = Manifest::load("naab.json");

    // 2. Add dependency if specified
    if (!package_name.empty()) {
        manifest.addDependency(package_name, version_spec);
        manifest.save();
    }

    // 3. Resolve dependencies
    DependencyResolver resolver(registry_client_);
    auto resolved = resolver.resolve(manifest.getDependencies());

    // 4. Generate lockfile
    Lockfile lockfile;
    lockfile.setDependencies(resolved);
    lockfile.save("naab.lock");

    // 5. Install packages
    Installer installer(registry_client_);
    for (const auto& [name, version] : resolved) {
        installer.install(name, version, "naab_modules/");
    }

    std::cout << "Installed " << resolved.size() << " packages\n";
}
```

---

### naab-pkg update

**Update dependencies to latest compatible versions:**

```bash
naab-pkg update
naab-pkg update http  # Update specific package
```

**Implementation:**

```cpp
void PackageManager::update(const std::string& package_name) {
    Manifest manifest = Manifest::load("naab.json");
    Lockfile lockfile = Lockfile::load("naab.lock");

    // Get current dependencies
    auto deps = manifest.getDependencies();

    if (!package_name.empty()) {
        // Update specific package
        if (deps.count(package_name) == 0) {
            throw Error("Package '" + package_name + "' not in dependencies");
        }

        // Re-resolve with updated constraints
        DependencyResolver resolver(registry_client_);
        auto resolved = resolver.resolveOne(package_name, deps[package_name]);

        // Update lockfile
        lockfile.updateDependency(package_name, resolved);
    } else {
        // Update all packages
        DependencyResolver resolver(registry_client_);
        auto resolved = resolver.resolve(deps);
        lockfile.setDependencies(resolved);
    }

    lockfile.save("naab.lock");

    // Reinstall
    install();
}
```

---

### naab-pkg publish

**Publish package to registry:**

```bash
naab-pkg publish
```

**Requirements:**
- Must be logged in: `naab-pkg login`
- Version must not exist in registry
- Package must pass validation

**Implementation:**

```cpp
void PackageManager::publish() {
    // 1. Load manifest
    Manifest manifest = Manifest::load("naab.json");

    // 2. Validate package
    validatePackage(manifest);

    // 3. Create tarball
    std::string tarball_path = createTarball(manifest);

    // 4. Upload to registry
    registry_client_.publishPackage(
        manifest.getName(),
        manifest.getVersion(),
        tarball_path,
        manifest
    );

    std::cout << "Published " << manifest.getName() << "@" << manifest.getVersion() << "\n";
}

void PackageManager::validatePackage(const Manifest& manifest) {
    // Check required fields
    if (manifest.getName().empty()) {
        throw Error("Package name is required");
    }

    if (manifest.getVersion().empty()) {
        throw Error("Package version is required");
    }

    // Check version format (semver)
    if (!isValidSemver(manifest.getVersion())) {
        throw Error("Invalid version: " + manifest.getVersion());
    }

    // Check main file exists
    if (!fs::exists(manifest.getMain())) {
        throw Error("Main file not found: " + manifest.getMain());
    }

    // Run tests
    if (manifest.hasScript("test")) {
        int exit_code = runScript("test");
        if (exit_code != 0) {
            throw Error("Tests failed, cannot publish");
        }
    }
}
```

---

### naab-pkg login

**Authenticate with registry:**

```bash
naab-pkg login
```

**Interactive:**
```
Username: alice
Password: ********
Email: alice@example.com

Logged in as alice
```

**Implementation:**

```cpp
void PackageManager::login() {
    std::string username = prompt("Username");
    std::string password = promptPassword("Password");
    std::string email = prompt("Email");

    // Authenticate with registry
    std::string token = registry_client_.authenticate(username, password, email);

    // Save token
    Config config;
    config.setAuthToken(token);
    config.setUsername(username);
    config.save();

    std::cout << "Logged in as " << username << "\n";
}
```

---

## Dependency Resolution

### Algorithm

**Goal:** Find versions that satisfy all constraints.

**Constraints:**
- Direct dependencies from naab.json
- Transitive dependencies from packages
- Version compatibility (semver)

**Algorithm (Simplified):**

```cpp
class DependencyResolver {
public:
    std::map<std::string, std::string> resolve(
        const std::map<std::string, std::string>& direct_deps
    ) {
        std::map<std::string, std::string> resolved;
        std::queue<std::pair<std::string, std::string>> to_process;

        // Start with direct dependencies
        for (const auto& [name, version_spec] : direct_deps) {
            to_process.push({name, version_spec});
        }

        while (!to_process.empty()) {
            auto [name, version_spec] = to_process.front();
            to_process.pop();

            // Skip if already resolved
            if (resolved.count(name)) {
                // Check if compatible
                if (!isCompatible(resolved[name], version_spec)) {
                    throw DependencyConflict(name, resolved[name], version_spec);
                }
                continue;
            }

            // Find best version
            std::string version = findBestVersion(name, version_spec);
            resolved[name] = version;

            // Get transitive dependencies
            auto package_info = registry_client_.getPackageInfo(name, version);
            for (const auto& [dep_name, dep_version_spec] : package_info.dependencies) {
                to_process.push({dep_name, dep_version_spec});
            }
        }

        return resolved;
    }

private:
    std::string findBestVersion(const std::string& name, const std::string& version_spec) {
        // Get all versions from registry
        auto versions = registry_client_.getVersions(name);

        // Filter by version spec
        std::vector<std::string> compatible;
        for (const auto& version : versions) {
            if (matchesVersionSpec(version, version_spec)) {
                compatible.push_back(version);
            }
        }

        if (compatible.empty()) {
            throw Error("No compatible version found for " + name + version_spec);
        }

        // Return highest compatible version
        std::sort(compatible.begin(), compatible.end(), compareSemver);
        return compatible.back();
    }

    bool matchesVersionSpec(const std::string& version, const std::string& spec) {
        if (spec == "*") return true;

        if (spec[0] == '^') {
            // Caret: compatible (same major)
            auto required = parseSemver(spec.substr(1));
            auto actual = parseSemver(version);
            return actual.major == required.major &&
                   (actual.minor > required.minor ||
                    (actual.minor == required.minor && actual.patch >= required.patch));
        }

        if (spec[0] == '~') {
            // Tilde: patch-level (same major.minor)
            auto required = parseSemver(spec.substr(1));
            auto actual = parseSemver(version);
            return actual.major == required.major &&
                   actual.minor == required.minor &&
                   actual.patch >= required.patch;
        }

        // Exact
        return version == spec;
    }
};
```

---

## Package Registry

### Option A: Central Registry (Recommended)

**Architecture:**

```
┌─────────────────────────────────┐
│    registry.naab.dev            │
│                                 │
│  - REST API                     │
│  - Package storage (S3)         │
│  - Metadata database            │
│  - User authentication          │
└─────────────────────────────────┘
```

**API:**

```
POST /api/v1/packages           # Publish package
GET  /api/v1/packages/:name     # Get package info
GET  /api/v1/packages/:name/:version  # Get specific version
GET  /api/v1/packages/:name/versions  # List versions
POST /api/v1/auth/login         # Authenticate
GET  /api/v1/search?q=http      # Search packages
```

**Benefits:**
- Central discovery
- Easy to use
- Package statistics
- npm-like experience

**Drawbacks:**
- Infrastructure cost
- Single point of failure
- Requires maintenance

### Option B: Git-Based Registry

**Architecture:**

Each package is a Git repository:

```
https://github.com/naab-packages/http.git
https://github.com/naab-packages/json.git
```

**naab.json:**
```json
{
  "dependencies": {
    "http": "github:naab-packages/http#v2.0.0",
    "json": "git+https://github.com/naab-packages/json.git#v1.5.0"
  }
}
```

**Benefits:**
- No central registry needed
- Git handles versioning
- Decentralized

**Drawbacks:**
- Slower (git clone overhead)
- No central search
- Harder to discover packages

### Recommendation: **Option A (Central Registry)**

**Rationale:**
- Better developer experience
- Easier discovery
- Standard approach (npm, PyPI, crates.io)

---

## Module System Integration

### Import from Packages

**Syntax:**

```naab
import "http"         // From naab_modules/http/
import "http/client"  // From naab_modules/http/client.naab
import "json"         // From naab_modules/json/
```

**Module Resolution:**

```cpp
class ModuleLoader {
    std::string resolvePath(const std::string& module_name) {
        // 1. Standard library
        if (module_name.starts_with("std/")) {
            return resolveStdModule(module_name);
        }

        // 2. node_modules (package)
        std::string package_path = "naab_modules/" + module_name;
        if (fs::exists(package_path)) {
            // Load main file from package manifest
            auto manifest = Manifest::load(package_path + "/naab.json");
            return package_path + "/" + manifest.getMain();
        }

        // 3. Relative path
        if (module_name.starts_with("./") || module_name.starts_with("../")) {
            return fs::canonical(module_name + ".naab");
        }

        throw ModuleNotFoundError(module_name);
    }
};
```

---

## Security

### Package Integrity

**Checksum Verification:**

```cpp
void Installer::verifyIntegrity(const std::string& tarball_path, const std::string& expected_hash) {
    // Compute SHA-512 of tarball
    std::string actual_hash = computeSHA512(tarball_path);

    if (actual_hash != expected_hash) {
        throw IntegrityError("Package integrity check failed");
    }
}
```

**Lockfile stores checksums:**

```json
{
  "http": {
    "version": "2.0.0",
    "integrity": "sha512-abc123..."
  }
}
```

### Code Signing (Future)

- Packages signed with GPG key
- Verify signature before installation
- Trust model (similar to apt/yum)

---

## Implementation Plan

### Week 1: Core Infrastructure (5 days)

- [ ] Implement Manifest class (naab.json parsing)
- [ ] Implement Lockfile class (naab.lock handling)
- [ ] Implement DependencyResolver (algorithm)
- [ ] Test: Resolve simple dependencies

### Week 2: Registry Client (5 days)

- [ ] Implement RegistryClient (HTTP API)
- [ ] Implement authentication
- [ ] Implement package fetching
- [ ] Test: Can fetch from registry

### Week 3: Installer (5 days)

- [ ] Implement Installer (download + extract)
- [ ] Implement integrity verification
- [ ] Implement naab_modules management
- [ ] Test: Install packages correctly

### Week 4: CLI (5 days)

- [ ] Implement CLI (init, install, update, publish, login)
- [ ] Command-line parsing
- [ ] User-friendly output
- [ ] Test: All commands work

### Week 5: Publisher (5 days)

- [ ] Implement package validation
- [ ] Implement tarball creation
- [ ] Implement publishing logic
- [ ] Test: Can publish package

### Week 6: Registry Setup (5 days)

- [ ] Set up registry server (if central)
- [ ] Database schema
- [ ] API implementation
- [ ] Test: Full end-to-end flow

**Total: 6 weeks**

---

## Testing Strategy

### Unit Tests

```cpp
TEST(DependencyResolverTest, ResolveSimple) {
    DependencyResolver resolver(&mock_registry);

    std::map<std::string, std::string> deps = {
        {"http", "^2.0.0"}
    };

    auto resolved = resolver.resolve(deps);

    ASSERT_EQ(resolved["http"], "2.0.3");  // Latest compatible
}

TEST(DependencyResolverTest, ResolveTransitive) {
    // http depends on net
    auto resolved = resolver.resolve({{"http", "^2.0.0"}});

    ASSERT_TRUE(resolved.count("net"));  // Transitive dep included
}
```

### Integration Tests

```bash
# Create test package
naab-pkg init
naab-pkg install http --save

# Verify naab.json updated
# Verify naab.lock created
# Verify naab_modules/http exists

# Test importing
echo 'import "http"' > test.naab
naab test.naab  # Should resolve import
```

---

## Success Metrics

### Phase 4.5 Complete When:

- [x] naab-pkg CLI working (init, install, update, publish, login)
- [x] Dependency resolution working
- [x] Package registry operational
- [x] Lockfile ensures reproducible builds
- [x] Module system integrates with packages
- [x] Security (integrity verification)
- [x] Documentation complete
- [x] 10+ packages published to test registry

---

## Conclusion

**Phase 4.5 Status: DESIGN COMPLETE**

A package manager will:
- Enable code reuse and sharing
- Grow the NAAb ecosystem
- Standard dependency management

**Implementation Effort:** 6 weeks

**Priority:** Medium-High (important for ecosystem)

**Dependencies:** Module system, registry infrastructure

Once implemented, NAAb will have package management on par with npm, cargo, or pip.
