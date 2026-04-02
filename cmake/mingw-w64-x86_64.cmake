# CMake toolchain file for cross-compiling to Windows x86_64
# using the llvm-mingw-w64 toolchain on Termux.
#
# Usage:
#   cmake -B build_win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#         -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TRIPLE "x86_64-w64-mingw32")

set(CMAKE_C_COMPILER   ${TRIPLE}-clang)
set(CMAKE_CXX_COMPILER ${TRIPLE}-clang++)
set(CMAKE_RC_COMPILER  ${TRIPLE}-windres)
set(CMAKE_AR           ${TRIPLE}-llvm-ar)
set(CMAKE_RANLIB       ${TRIPLE}-llvm-ranlib)

# MinGW sysroot (headers + import libs)
set(CMAKE_FIND_ROOT_PATH
    $ENV{PREFIX}/${TRIPLE}
    $ENV{PREFIX}
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Disable hardening flags that require GCC/Clang POSIX extensions
set(ENABLE_HARDENING OFF CACHE BOOL "" FORCE)
set(ENABLE_ASAN      OFF CACHE BOOL "" FORCE)
set(ENABLE_UBSAN     OFF CACHE BOOL "" FORCE)
