#!/usr/bin/env bash
# ============================================================
# test_compile_tempdir_cleanup.sh -- a compiled polyglot block leaves no
# naab_<lang>_* directory behind in TMPDIR, however the executor is left
#
# THE FAILURE THIS CATCHES
#
# Each compiled-language executor makes a private mkdtemp directory per block
# (V-RCE-004/V-RCE-010) and used to spell out its cleanup on every exit path.
# The spelling was short in three ways, all measured on fecb13e:
#
#   * C#, Nim, Julia and Zig removed the files and never the directory, on
#     every path -- including when the compiler is not installed at all,
#     because the directory is made before the compiler is looked for.
#   * C++'s two cache-miss paths kept the directory on purpose, under a
#     comment saying the cache needed it. The cache copies the binary into
#     ~/.naab/cache; nothing ever read the directory again.
#   * Rust and C++ have no try/catch, so an exception -- a --timeout firing
#     during the compile is the ordinary one -- skipped the cleanup.
#
# (Reported: one container had accumulated 863.) Every executor now holds the
# directory in a ScopedTempDir (include/naab/scoped_temp_dir.h), which removes
# it in its destructor.
#
# HOW IT IS MEASURED. Each run gets its own empty TMPDIR, and afterwards the
# test asserts no naab_* directory is left in it. That alone cannot fail for a
# directory created somewhere else, so every arm also needs to learn WHICH
# directory the executor made, and assert that one is gone:
#
#   stub arms  A stub toolchain placed first on PATH records its argv (every
#              compiler and runner is handed a path inside the directory) and
#              then fails (S) or hangs until --timeout fires (H). These run
#              whether or not the real toolchain is installed, which is the
#              point: CI has no C#, Nim, Julia or Zig, and those four were the
#              worst leakers. The hang arms are the exception path.
#   real arms  The real toolchain, where installed, compiles a block that
#              prints its own executable's path. Corroboration that the stub
#              arms did not test a path real compilers never take.
#
# An arm whose recorded path is missing reports UNMEASURABLE, never PASS: no
# record means the block never reached the executor, so a clean TMPDIR proves
# nothing.
#
#   TD-00  every mkdtemp in src/ goes through ScopedTempDir, and every prefix
#          ScopedTempDir is given has a case here (enumerated from the source,
#          so a new executor fails this until it gets one)
#   TD-01  CONTROL: the leftover probe reports a directory planted in TMPDIR
#   TD-S-* stub toolchain fails          TD-H-* stub toolchain hangs
#   TD-R-* real toolchain
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "$3" | sed 's/^/       | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Compiled-language temp directories are removed on every exit ==="

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        skip "TD-00" "compiled-language executors are not built on Windows -- UNMEASURABLE"
        report; exit 0 ;;
esac
if [ ! -x "$NAAB" ]; then
    echo "  naab-lang not built at $NAAB"; exit 1
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-tdclean-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# Each run moves HOME (so the C++ compile cache starts empty and every arm is
# a cache miss). rustup's rustc proxy and go's build cache default under HOME,
# so pin them to where they really are first.
export RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}" CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
if command -v go >/dev/null 2>&1; then
    GOCACHE="${GOCACHE:-$(go env GOCACHE 2>/dev/null)}"; export GOCACHE
    GOPATH="${GOPATH:-$(go env GOPATH 2>/dev/null)}"; export GOPATH
fi

mkdir -p "$W/proj"
cat > "$W/proj/govern.json" <<'EOF'
{
  "version": "4.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" }
}
EOF

# ---- programs ------------------------------------------------------------
# One per executor ENTRY POINT, not per language: the directory is per
# function, and the C++ adapter alone has four. "exec" programs reach
# Executor::execute() through a persistent runtime (code starting with a
# statement keyword); the rest reach executeWithReturn(). Code that only stubs
# will see need not compile.
prog() {  # $1 = shape -> program text on stdout
    case "$1" in
        cpp-expr)   # executeWithReturn, expression (cache-miss path)
            printf 'main {\n    let p = <<cpp\n#include <filesystem>\nstd::filesystem::read_symlink("/proc/self/exe").string()\n>>\n    print(p)\n}\n' ;;
        cpp-main)   # executeWithReturn, int main() (cache-miss path)
            printf 'main {\n    let p = <<cpp\n#include <iostream>\nint main(int argc, char** argv) { std::cout << argv[0] << std::endl; return 0; }\n>>\n    print(p)\n}\n' ;;
        cpp-stmt)   # executeWithReturn -> execute(), wrapped statement
            printf 'main {\n    <<cpp\nstd::cout << "x" << std::endl;\n>>\n}\n' ;;
        cpp-exec)   # execute(), int main()
            printf 'main {\n    runtime c = cpp.start()\n    c.exec(<<cpp\nconst int k = 0;\nint main() { return k; }\n>>)\n}\n' ;;
        cpp-block)  # CppExecutor::compileBlock (tree-walker only: use BLOCK-...)
            printf 'use BLOCK-CPP-TDCLEAN as probe\nmain {\n    print("loaded")\n}\n' ;;
        rust-ret)
            printf 'main {\n    let p = <<rust\nstd::env::current_exe().unwrap().display()\n>>\n    print(p)\n}\n' ;;
        rust-exec)
            printf 'main {\n    runtime r = rust.start()\n    r.exec(<<rust\nfor _i in 0..1 {}\n>>)\n}\n' ;;
        go-ret)
            printf 'main {\n    let p = <<go\npackage main\nimport ("fmt"; "os")\nfunc main() { fmt.Println(os.Args[0]) }\n>>\n    print(p)\n}\n' ;;
        go-exec)
            printf 'main {\n    runtime g = go.start()\n    g.exec(<<go\nfor i := 0; i < 1; i++ {}\n>>)\n}\n' ;;
        cs-ret)
            printf 'main {\n    let p = <<csharp\nSystem.Reflection.Assembly.GetExecutingAssembly().Location\n>>\n    print(p)\n}\n' ;;
        cs-exec)
            printf 'main {\n    runtime s = csharp.start()\n    s.exec(<<csharp\nfor (int i = 0; i < 1; i++) {}\n>>)\n}\n' ;;
        nim-ret)
            printf 'main {\n    let p = <<nim\nimport os\ngetAppFilename()\n>>\n    print(p)\n}\n' ;;
        julia-ret)
            printf 'main {\n    let p = <<julia\nPROGRAM_FILE\n>>\n    print(p)\n}\n' ;;
        zig-ret)
            printf 'main {\n    let p = <<zig\nconst x: i32 = 1;\n>>\n    print(p)\n}\n' ;;
    esac
}

# shape -> "<tool the stub replaces> <directory prefix> [engine flag]"
# For Julia the stub replaces the RUNNER: there is no compile step.
SHAPES="cpp-expr cpp-main cpp-stmt cpp-exec cpp-block rust-ret rust-exec go-ret go-exec cs-ret cs-exec nim-ret julia-ret zig-ret"
shape_info() {
    case "$1" in
        cpp-block)  echo "clang++ naab_cpp_bl_ --tree-walk" ;;
        cpp-*)      echo "g++ naab_cpp_" ;;
        rust-*)     echo "rustc naab_rust_" ;;
        go-*)       echo "go naab_go_" ;;
        cs-*)       echo "mcs naab_cs_" ;;
        nim-*)      echo "nim naab_nim_" ;;
        julia-*)    echo "julia naab_julia_" ;;
        zig-*)      echo "zig naab_zig_" ;;
    esac
}
# Prefixes with no shape above. Empty: every prefix in src/ has a case.
UNCOVERED_PREFIXES=""

# ---- the probe -----------------------------------------------------------
leftovers() {  # $1 = TMPDIR of the run -> every naab_* directory still in it
    find "$1" -mindepth 1 -maxdepth 1 -type d -name 'naab_*' 2>/dev/null | sort
}

# run_naab ARM_DIR PROGRAM_FILE STUB_DIR_OR_EMPTY [naab args...] -> OUT, RC
run_naab() {
    local arm="$1" program="$2" stubdir="$3"; shift 3
    local path_prefix=""
    [ -n "$stubdir" ] && path_prefix="$stubdir:"
    OUT="$(cd "$arm" && PATH="$path_prefix$PATH" TMPDIR="$arm/tmp" HOME="$arm/home" \
           timeout 120 "$NAAB" "$@" "$program" 2>&1)"
    RC=$?
}

# Governance is discovered from the PROGRAM's directory, so each arm carries
# its own copy of the config beside its program.
new_arm() {  # $1 = arm id -> prints the arm directory, freshly made
    local d="$W/arm-$1"
    rm -rf "$d"; mkdir -p "$d/tmp" "$d/home/.naab/language/blocks/library/cpp"
    cp "$W/proj/govern.json" "$d/govern.json"
    printf 'extern "C" int tdclean_probe() { return 7; }\n' \
        > "$d/home/.naab/language/blocks/library/cpp/BLOCK-CPP-TDCLEAN.cpp"
    echo "$d"
}

make_stub() {  # $1 = arm dir, $2 = tool, $3 = fail|hang
    local tail='exit 1'
    [ "$3" = hang ] && tail='exec sleep 60'
    mkdir -p "$1/stub"
    cat > "$1/stub/$2" <<EOF
#!/bin/sh
printf '%s\n' "\$@" > "$1/rec"
$tail
EOF
    chmod +x "$1/stub/$2"
}

# Asserts that DIR (the directory this arm's executor made) is gone and that
# nothing else is left either.
check_clean() {  # $1 = id, $2 = arm dir, $3 = recorded dir, $4 = what ran
    local left
    left="$(leftovers "$2/tmp")"
    if [ -e "$3" ] || [ -n "$left" ]; then
        bad "$1" "$4: compile directory left behind" \
            "made: ${3#"$2"/tmp/}$( [ -e "$3" ] && echo ' (still exists)')
left: $(echo "$left" | sed "s|$2/tmp/||" | tr '\n' ' ')"
    else
        ok "$1" "$4: ${3##*/} made and removed"
    fi
}

# ---- TD-00: enumerate from the source --------------------------------------
RAW="$(grep -rnE '\bmkdtemp[[:space:]]*\(' "$REPO/src" "$REPO/include" 2>/dev/null \
       | grep -v 'include/naab/scoped_temp_dir.h' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true)"
PREFIXES="$(grep -rhoE '(ScopedTempDir [A-Za-z_0-9]+\(|\.emplace\()[^;]*"naab_[a-z_]+"' "$REPO/src" 2>/dev/null \
            | grep -oE '"naab_[a-z_]+"' | tr -d '"' | sort -u)"
if [ -n "$RAW" ]; then
    bad "TD-00" "mkdtemp called outside ScopedTempDir -- that directory has no owner" "$RAW"
elif [ "$(echo "$PREFIXES" | grep -c .)" -lt 2 ]; then
    bad "TD-00" "found $(echo "$PREFIXES" | grep -c .) ScopedTempDir prefixes in src/ -- the enumeration is broken"
else
    MISSING=""
    for p in $PREFIXES; do
        covered=no
        for s in $SHAPES; do
            set -- $(shape_info "$s")
            [ "$2" = "$p" ] && covered=yes
        done
        case " $UNCOVERED_PREFIXES " in *" $p "*) covered=listed ;; esac
        [ "$covered" = no ] && MISSING="$MISSING $p"
    done
    if [ -n "$MISSING" ]; then
        bad "TD-00" "ScopedTempDir prefix with no case in this test:$MISSING"
    else
        ok "TD-00" "no raw mkdtemp; all $(echo "$PREFIXES" | grep -c .) prefixes have a case: $(echo $PREFIXES)"
    fi
fi

# ---- TD-01: the probe can see a leftover ------------------------------------
CTL="$(new_arm ctl)"
mkdir -p "$CTL/tmp/naab_cpp_PLANTD"
case "$(leftovers "$CTL/tmp")" in
    *naab_cpp_PLANTD*) ok "TD-01" "CONTROL: a planted naab_cpp_ directory is reported" ;;
    *)                 bad "TD-01" "CONTROL: the leftover probe missed a planted directory -- every PASS below is void" ;;
esac

# ---- stub arms -------------------------------------------------------------
for mode in fail hang; do
    tag=S; [ "$mode" = hang ] && tag=H
    for shape in $SHAPES; do
        set -- $(shape_info "$shape"); tool="$1"; prefix="$2"; engine="${3:-}"
        id="TD-$tag-$shape"
        arm="$(new_arm "$tag-$shape")"
        make_stub "$arm" "$tool" "$mode"
        prog "$shape" > "$arm/p.naab"
        if [ "$mode" = hang ]; then
            run_naab "$arm" "$arm/p.naab" "$arm/stub" $engine --timeout 2
        else
            run_naab "$arm" "$arm/p.naab" "$arm/stub" $engine
        fi
        made="$(grep -o -m1 "$arm/tmp/${prefix}[A-Za-z0-9]\{6\}" "$arm/rec" 2>/dev/null)"
        # The stub is the arm's only dependency on the machine, so a stub
        # that was never called means the harness is broken, not that the
        # toolchain is missing -- and a broken harness reporting SKIP is how
        # a leak stays invisible. Unmeasurable here is a FAILURE.
        if [ -z "$made" ]; then
            bad "$id" "stub $tool was never handed a $prefix directory -- UNMEASURABLE, harness broken (rc=$RC)" \
                "$(echo "$OUT" | grep -v '^\[governance\]' | grep -m3 .)"
            continue
        fi
        check_clean "$id" "$arm" "$made" "stub $tool ${mode}s"
    done
done

# ---- real-toolchain arms ---------------------------------------------------
# Only shapes whose block prints its own path, and only toolchains whose
# snippet has been run for real. Julia and Zig have none: neither was
# available where this was written, and an unrun snippet that misbehaves
# would read as UNMEASURABLE forever. Their stub arms stand alone.
for shape in cpp-expr cpp-main rust-ret go-ret cs-ret nim-ret; do
    set -- $(shape_info "$shape"); tool="$1"; prefix="$2"
    id="TD-R-$shape"
    need="$tool"; [ "$tool" = mcs ] && need="mcs mono"
    missing=""
    for t in $need; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
    if [ -n "$missing" ]; then
        skip "$id" "toolchain absent ($missing) -- UNMEASURABLE"
        continue
    fi
    arm="$(new_arm "R-$shape")"
    prog "$shape" > "$arm/p.naab"
    run_naab "$arm" "$arm/p.naab" ""
    made="$(echo "$OUT" | grep -o -m1 "$arm/tmp/${prefix}[A-Za-z0-9]\{6\}")"
    if [ -z "$made" ]; then
        skip "$id" "block did not report a $prefix path -- UNMEASURABLE (rc=$RC: $(echo "$OUT" | grep -v '^\[governance\]' | grep -m1 . | cut -c1-100))"
        continue
    fi
    check_clean "$id" "$arm" "$made" "real $tool"
done

report
[ "$FAIL" -eq 0 ]
