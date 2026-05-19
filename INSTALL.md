# Installing NAAb

## Pre-built Binaries (Recommended)

Download the latest release from [GitHub Releases](https://github.com/b-macker/NAAb/releases):

| Platform | File |
|----------|------|
| Linux x86_64 | `naab-linux-x86_64.tar.gz` |
| Linux aarch64 | `naab-linux-aarch64.tar.gz` |
| macOS arm64 | `naab-macos-arm64.tar.gz` |
| Windows x86_64 | `naab-windows-x86_64.zip` |

```bash
# Linux/macOS
tar xzf naab-linux-x86_64.tar.gz
sudo mv naab-lang naab-gov /usr/local/bin/

# Verify
naab-lang --version
```

## Build from Source

### Prerequisites

- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- CMake 3.16+
- SQLite3 development libraries
- OpenSSL development libraries
- libcurl development libraries
- Python3 development libraries (optional, for polyglot)

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install cmake ninja-build libsqlite3-dev python3-dev \
  libssl-dev libffi-dev libcurl4-openssl-dev pkg-config

git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cp build/naab-lang build/naab-gov /usr/local/bin/
```

### macOS

```bash
brew install cmake ninja openssl@3 sqlite3 pkg-config curl

git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build -j$(sysctl -n hw.ncpu)
cp build/naab-lang build/naab-gov /usr/local/bin/
```

### Windows

```powershell
# Install vcpkg dependencies
vcpkg install sqlite3 openssl curl --triplet x64-windows

git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# Add build/ to PATH
```

### Termux (Android)

```bash
pkg install cmake ninja clang libsqlite python openssl libcurl pkg-config

git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cp build/naab-lang build/naab-gov $PREFIX/bin/
```

## Verify Installation

```bash
naab-lang --version    # Should print version info
naab-gov --version     # Should print version info
```
