#!/usr/bin/env bash
# test_a148_widenings.sh -- two block checks read the C forms they missed
# (A148; the owner's decisions of 2026-11-11)
#
# Measured against outside labels (Bundle 13 PR 2, Juliet): code_quality.
# no_secrets flagged 0 of 112 files whose credential is a C macro
#   #define PASSWORD "..."
# because every assignment pattern wants `name = "..."`, and restrictions.crypto
# flagged 0 of 126 files that pick a weak algorithm by its CryptoAPI constant
#   CryptCreateHash(h, CALG_MD5, ...)
# because `\bmd5\b` does not match inside CALG_MD5 (`_` is a word character).
#
# Both widenings are tightenings. Each is as narrow as its catch allows:
#   secrets  the macro's NAME must END in a credential word (PASSWORD, PASSWD,
#            SECRET, TOKEN, API_KEY, PRIVATE_KEY, ACCESS_KEY, SECRET_KEY,
#            CRYPTO_KEY, ENCRYPTION_KEY, SIGNING_KEY) and its value must be a
#            string literal of 8..200 characters with no whitespace. So
#            PASSWORD_PROMPT, TOKEN_FILE, a number, a call and a sentence pass.
#   crypto   the named constants only: CALG_MD2, CALG_MD4, CALG_MD5, CALG_SHA,
#            CALG_SHA1 (hashes); CALG_DES, CALG_3DES, CALG_3DES_112, CALG_DESX,
#            CALG_RC2, CALG_RC4, CALG_RC5 (ciphers). Not rand(). A config that
#            lists its own weak_hashes / weak_ciphers gets a constant only for
#            an algorithm it lists.
#
# No literal credential is written here: each value is built from fragments.
#
# Arms
#   SEC-00   control: the checker flags an assignment-form credential
#   SEC-01   a credential-named #define with a string literal is flagged
#            (8 spellings)
#   SEC-02   not flagged: a name that only CONTAINS the word, a number, a call,
#            a sentence, a short value, an unrelated name (8 spellings)
#   SEC-03   real run: a C++ block carrying such a #define is refused (exit 3)
#   SEC-03c  control: the same block without the #define runs and returns its
#            value
#   SEC-04   control: with no_secrets absent the #define block is not flagged
#   CRY-00   control: the checker flags hashlib.md5
#   CRY-01   each named CryptoAPI constant is flagged (12)
#   CRY-02   not flagged: CALG_SHA_256, CALG_SHA_512, CALG_AES_256,
#            CALG_RSA_SIGN, longer identifiers that hold a constant's name at
#            their start, middle or end, a constant inside a string literal
#   CRY-03   a config listing weak_hashes ["md5"]: CALG_MD5 flagged, CALG_SHA1
#            and CALG_MD2 not
#   CRY-04   a config listing weak_ciphers ["rc4"]: CALG_RC4 flagged, CALG_DES
#            and CALG_3DES not
#   CRY-06   a config listing weak_ciphers ["des"]: the 3DES and DESX constants
#            are flagged with it, CALG_RC4 is not
#   CRY-05   control: with restrictions.crypto absent CALG_MD5 is not flagged
#
# Kill table (each mutant observed to turn its arm red; run by hand)
#   the macro pattern removed                      -> SEC-01, SEC-03
#   the macro's name need not end in the word      -> SEC-02
#   the macro's value may hold whitespace          -> SEC-02
#   no minimum length for the value                -> SEC-02
#   the wide-literal prefix dropped                -> SEC-01
#   hash constants not added                       -> CRY-01, CRY-03
#   cipher constants not added                     -> CRY-01, CRY-04, CRY-06
#   a config's own list ignored                    -> CRY-03, CRY-04, CRY-06
#   the des family not followed                    -> CRY-06
#   list names compared with case                  -> CRY-06
#   no boundary after a constant                   -> CRY-02
#   no boundary before a constant                  -> CRY-02 (survived the first
#       draft: the only longer identifier also had a suffix; MY_CALG_MD5 added)
#
# Red first (observed, master 9b616789): SEC-01, SEC-03, CRY-01, CRY-03 and
# CRY-04 fail; the controls and SEC-02, CRY-02 pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_a148_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
skip() { UNM=$((UNM+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
has() { grep -qF -- "$2" <<<"$1"; }

if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "  FAIL [setup] naab-lang or naab-gov not built at $(dirname "$NAAB")"; echo "Results: 0 passed, 1 failed, 0 skipped"; exit 1
fi

SEC_RULE="code_quality.no_secrets"
CRY_RULE="restrictions.crypto"
# A value shaped like a credential, never written whole.
V1="Abcd"; V2="1234"; V3="!xyZ"
VAL="${V1}${V2}${V3}"

mkdir -p "$W/sec" "$W/nosec" "$W/cry" "$W/nocry" "$W/hash" "$W/ciph" "$W/fam"
BASE='"mode": "enforce", "security": {"sandbox_level": "elevated"}'
printf '{%s, "code_quality": {"no_secrets": {"level": "hard"}}}\n' "$BASE" > "$W/sec/govern.json"
printf '{%s}\n' "$BASE" > "$W/nosec/govern.json"
printf '{%s, "restrictions": {"crypto": {"level": "hard"}}}\n' "$BASE" > "$W/cry/govern.json"
printf '{%s}\n' "$BASE" > "$W/nocry/govern.json"
printf '{%s, "restrictions": {"crypto": {"level": "hard", "weak_hashes": ["md5"]}}}\n' "$BASE" > "$W/hash/govern.json"
printf '{%s, "restrictions": {"crypto": {"level": "hard", "weak_ciphers": ["rc4"]}}}\n' "$BASE" > "$W/ciph/govern.json"
printf '{%s, "restrictions": {"crypto": {"level": "hard", "weak_ciphers": ["DES"]}}}\n' "$BASE" > "$W/fam/govern.json"

flagged() {   # $1 = config dir, $2 = language, $3 = rule, stdin = block; 0 when the rule fires
    local out
    out="$("$GOV" check --language "$2" --config "$W/$1/govern.json" 2>&1)"
    has "$out" "\"rule\": \"$3\""
}
c_block() {   # $1 = one line of C
    printf '#include <stdio.h>\n%s\nint f(void) { return 1; }\n' "$1"
}

# --- secrets -------------------------------------------------------------------
if ! printf 'password = "%s"\nprint(1)\n' "$VAL" | flagged sec python "$SEC_RULE"; then
    fail SEC-00 "the checker does not flag an assignment-form credential: it is not judging" ""
    fail SEC-01 "the checker is not judging" ""
    fail SEC-02 "the checker is not judging, so an unflagged row means nothing" ""
else
    pass SEC-00 "control: an assignment-form credential is flagged"
    missed=""
    while IFS= read -r t; do
        [ -z "$t" ] && continue
        line="${t//@V@/$VAL}"
        c_block "$line" | flagged sec cpp "$SEC_RULE" || missed="$missed [$t]"
    done <<'EOF'
#define PASSWORD "@V@"
#define DB_PASSWORD "@V@"
# define ADMIN_PASSWD "@V@"
#define CRYPTO_KEY "@V@"
#define SERVICE_API_KEY "@V@"
#define AUTH_TOKEN "@V@"
#define client_secret "@V@"
#define SIGNING_KEY L"@V@"
EOF
    if [ -z "$missed" ]; then pass SEC-01 "a credential-named #define with a string literal is flagged (8 spellings)"
    else fail SEC-01 "not flagged:$missed" ""; fi

    wrong=""
    while IFS= read -r t; do
        [ -z "$t" ] && continue
        line="${t//@V@/$VAL}"
        c_block "$line" | flagged sec cpp "$SEC_RULE" && wrong="$wrong [$t]"
    done <<'EOF'
#define PASSWORD_PROMPT "@V@"
#define TOKEN_FILE "@V@"
#define PASSWORD_MAX_LEN 64
#define PASSWORD getenv("APP_PASSWORD")
#define PASSWORD "Enter the password now"
#define PASSWORD "short"
#define GREETING "@V@"
#define MAX_TOKENS 4096
EOF
    if [ -z "$wrong" ]; then pass SEC-02 "names that only contain the word, numbers, calls, sentences and short values are not flagged (8 spellings)"
    else fail SEC-02 "flagged though it is not a credential macro:$wrong" ""; fi
fi

if c_block "#define DB_PASSWORD \"$VAL\"" | flagged nosec cpp "$SEC_RULE"; then
    fail SEC-04 "the #define block is flagged with no_secrets absent" ""
else pass SEC-04 "control: with no_secrets absent the #define block is not flagged"; fi

# real runs
run_cpp() {   # $1 = dir name, $2 = extra first line of the block ("" for none); sets OUT, RC
    local d="$W/$1"; mkdir -p "$d"
    cp "$W/sec/govern.json" "$d/govern.json"
    {
        echo 'main {'
        echo '    let r = <<cpp'
        [ -n "$2" ] && echo "$2"
        echo 'int total = 2 + 4;'
        echo 'total'
        echo '>>'
        echo '    print("VALUE=" + r)'
        echo '}'
    } > "$d/p.naab"
    OUT="$(cd "$d" && timeout 120 "$NAAB" run p.naab 2>&1)"; RC=$?
}
run_cpp probe ""
if [ "$RC" != 0 ] || ! has "$OUT" "VALUE=6"; then
    skip SEC-03 "a plain C++ block does not run here (rc=$RC)"
    skip SEC-03c "a plain C++ block does not run here (rc=$RC)"
else
    pass SEC-03c "control: the block without the #define runs and returns its value"
    run_cpp sec03 "#define DB_PASSWORD \"$VAL\""
    if [ "$RC" = 3 ] && has "$OUT" "$SEC_RULE" && ! has "$OUT" "VALUE=6"; then
        pass SEC-03 "real run: a C++ block carrying a credential #define is refused (exit 3)"
    else fail SEC-03 "the block with a credential #define was not refused (rc=$RC)" "$OUT"; fi
fi

# --- crypto --------------------------------------------------------------------
use_c() {   # $1 = a CryptoAPI constant, or any expression
    printf '#include <windows.h>\nint f(HCRYPTPROV p, HCRYPTHASH *h) { return CryptCreateHash(p, %s, 0, 0, h); }\n' "$1"
}
if ! printf 'import hashlib\nh = hashlib.md5(b"x")\nprint(h)\n' | flagged cry python "$CRY_RULE"; then
    fail CRY-00 "the checker does not flag hashlib.md5: it is not judging" ""
    fail CRY-01 "the checker is not judging" ""
    fail CRY-02 "the checker is not judging, so an unflagged row means nothing" ""
    fail CRY-03 "the checker is not judging" ""
    fail CRY-04 "the checker is not judging" ""
    fail CRY-06 "the checker is not judging" ""
else
    pass CRY-00 "control: hashlib.md5 is flagged"
    missed=""
    for c in CALG_MD2 CALG_MD4 CALG_MD5 CALG_SHA CALG_SHA1 CALG_DES CALG_3DES CALG_3DES_112 CALG_DESX CALG_RC2 CALG_RC4 CALG_RC5; do
        use_c "$c" | flagged cry cpp "$CRY_RULE" || missed="$missed $c"
    done
    if [ -z "$missed" ]; then pass CRY-01 "each named CryptoAPI constant is flagged (12)"
    else fail CRY-01 "not flagged:$missed" ""; fi

    wrong=""
    for c in CALG_SHA_256 CALG_SHA_512 CALG_AES_256 CALG_RSA_SIGN MY_CALG_MD5_SLOT MY_CALG_MD5 CALG_MD5X; do
        use_c "$c" | flagged cry cpp "$CRY_RULE" && wrong="$wrong $c"
    done
    printf 'const char *name(void) { return "CALG_MD5"; }\n' | flagged cry cpp "$CRY_RULE" && wrong="$wrong [in a string]"
    if [ -z "$wrong" ]; then pass CRY-02 "strong constants, longer identifiers and a constant inside a string are not flagged (8)"
    else fail CRY-02 "flagged though it names no weak algorithm:$wrong" ""; fi

    r=""
    use_c CALG_MD5 | flagged hash cpp "$CRY_RULE" || r="$r CALG_MD5-not-flagged"
    use_c CALG_SHA1 | flagged hash cpp "$CRY_RULE" && r="$r CALG_SHA1-flagged"
    use_c CALG_MD2 | flagged hash cpp "$CRY_RULE" && r="$r CALG_MD2-flagged"
    if [ -z "$r" ]; then pass CRY-03 "weak_hashes [md5]: CALG_MD5 flagged, CALG_SHA1 and CALG_MD2 not"
    else fail CRY-03 "a listed hash set is not followed:$r" ""; fi

    r=""
    use_c CALG_RC4 | flagged ciph cpp "$CRY_RULE" || r="$r CALG_RC4-not-flagged"
    use_c CALG_DES | flagged ciph cpp "$CRY_RULE" && r="$r CALG_DES-flagged"
    use_c CALG_3DES | flagged ciph cpp "$CRY_RULE" && r="$r CALG_3DES-flagged"
    if [ -z "$r" ]; then pass CRY-04 "weak_ciphers [rc4]: CALG_RC4 flagged, CALG_DES and CALG_3DES not"
    else fail CRY-04 "a listed cipher set is not followed:$r" ""; fi

    r=""
    for c in CALG_DES CALG_3DES CALG_3DES_112 CALG_DESX; do
        use_c "$c" | flagged fam cpp "$CRY_RULE" || r="$r $c-not-flagged"
    done
    use_c CALG_RC4 | flagged fam cpp "$CRY_RULE" && r="$r CALG_RC4-flagged"
    if [ -z "$r" ]; then pass CRY-06 "weak_ciphers [DES]: the 3DES and DESX constants are flagged with it, CALG_RC4 is not"
    else fail CRY-06 "the des family is not followed:$r" ""; fi
fi

if use_c CALG_MD5 | flagged nocry cpp "$CRY_RULE"; then
    fail CRY-05 "CALG_MD5 is flagged with restrictions.crypto absent" ""
else pass CRY-05 "control: with restrictions.crypto absent CALG_MD5 is not flagged"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM skipped"
[ "$FAIL" = 0 ]
