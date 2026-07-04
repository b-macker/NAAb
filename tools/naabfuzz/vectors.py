"""Fixed deterministic arithmetic vectors — hand-audited expected outputs.

Each vector is (naab expression, expected print line or ERROR:<category>).
Run on both engines. These lock in the empirically audited semantics the
oracle is built on, so an engine regression OR an oracle drift shows up here.
"""

from __future__ import annotations

from .runner import run_engine
from .triage import classify_error

VECTORS = [
    # int arithmetic + checked overflow
    ("1 + 2", "3"),
    ("2147483647 + 1", "ERROR:integer_overflow"),
    ("-2147483647 - 2", "ERROR:integer_overflow"),
    ("(-2147483647 - 1) * -1", "ERROR:integer_overflow"),
    ("46341 * 46341", "ERROR:integer_overflow"),
    ("46340 * 46340", "2147395600"),
    # division: ALWAYS double (unified semantics)
    ("7 / 2", "3.5"),
    ("8 / 2", "4"),
    ("1 / 3", "0.333333333333333"),
    ("(-2147483647 - 1) / -1", "2147483648"),
    ("1 / 0", "ERROR:division_by_zero"),
    ("1.5 / 0.0", "ERROR:division_by_zero"),
    # modulo: int-only, truncated toward zero
    ("-7 % 3", "-1"),
    ("7 % -3", "1"),
    ("(-7) % 2", "-1"),
    ("7 % (-2)", "1"),
    ("(-2147483647 - 1) % -1", "0"),
    ("5 % 0", "ERROR:modulo_by_zero"),
    ("1.5 % 2", "ERROR:type_error"),
    # float formatting: %.15g
    ("0.1 + 0.2", "0.3"),
    ("1.0", "1"),
    ("-0.0", "-0"),
    ("10000000000.0 * 10000000000.0", "1e+20"),
    ("123456789.123456789", "123456789.123457"),
    # mixed promotion
    ("1 + 2.5", "3.5"),
    ("2 * 0.5", "1"),
    # spellings
    ("true", "true"),
    ("false", "false"),
    ("null", "null"),
    # string semantics
    ('"x" + 1', "x1"),
    ('"x" + 3.5', "x3.5"),
    ('"b" + true', "btrue"),
    ('"ab" * 3', "ababab"),
    ('3 * "ab"', "ababab"),
    # comparisons
    ("1 == 1.0", "true"),
    ('"a" == 1', "false"),
    ("2 < 10", "true"),
    ('"2" < "10"', "false"),
    # null coalescing
    ("null ?? 5", "5"),
    ("7 ?? 5", "7"),
    # BOOL-001 unification: bool is numeric (1/0) in arithmetic + ordering
    ("true + 1", "2"),
    ("1 + true", "2"),
    ("true - 1", "0"),
    ("true * 3", "3"),
    ("true / 2", "0.5"),
    ("true % 2", "1"),
    ("5 % true", "0"),
    ("-true", "-1"),
    ("true < 2", "true"),
    ("false < true", "true"),
    ("true + 1.5", "2.5"),
    ("true == 1", "false"),  # equality stays type-aware
    ('"s" + true', "strue"),  # concat formats bool as text
    # CMP-001 unification: mixed string/number ordering is a type error
    ('"a" < 1', "ERROR:type_error"),
    ('1 < "a"', "ERROR:type_error"),
    ('"2" > 1', "ERROR:type_error"),
    ("true < \"a\"", "ERROR:type_error"),
    # FEAT-001 unification: string subscript + negative index wrap
    ('"hello"[1]', "e"),
    ('"hello"[-1]', "o"),
    ('"abc"[10]', "ERROR:index_error"),
    ("[10, 20, 30][-1]", "30"),
    ("[10, 20, 30][1 + 1]", "30"),
]


def run_vectors(naab_bin: str, verbose=print):
    failures = 0
    for expr, want in VECTORS:
        src = "main {\n    print(%s)\n}\n" % expr
        for tree_walk in (False, True):
            engine = "tw" if tree_walk else "vm"
            r = run_engine(naab_bin, src, tree_walk)
            if want.startswith("ERROR:"):
                cat = want.split(":", 1)[1]
                ok = r.rc == 1 and classify_error(r.error_text) == cat
                got = "rc=%d %s" % (r.rc, classify_error(r.error_text))
            else:
                ok = r.rc == 0 and r.out_lines == [want]
                got = "rc=%d %r" % (r.rc, r.out_lines[:2])
            if not ok:
                failures += 1
                verbose("  VECTOR FAIL [%s] %s => %s (want %s)"
                        % (engine, expr, got, want))
    return len(VECTORS) * 2, failures
