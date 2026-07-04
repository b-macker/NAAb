"""Metamorphic property suite.

Each property builds small NAAb programs from seeded value tuples, uses the
exact oracle (Fraction track where needed) to decide whether the identity
holds without float caveats, and asserts both engines print the expected
witness. This catches semantic bugs that a single fixed test never would.
"""

from __future__ import annotations

import random
from fractions import Fraction
from typing import List

from .oracle import INT32_MIN, INT32_MAX, fmt_double
from .runner import run_engine

PASS_LINE = "OK"


class PropertyFailure(Exception):
    pass


def _int_pool(rng: random.Random, n: int) -> List[int]:
    edges = [0, 1, -1, 7, -7, 100, INT32_MAX, INT32_MIN, INT32_MAX - 1,
             INT32_MIN + 1, 32768, -32768]
    vals = [rng.choice(edges) if rng.random() < 0.5
            else rng.randint(-10 ** 6, 10 ** 6) for _ in range(n)]
    return vals


def _fits32(x: int) -> bool:
    return INT32_MIN <= x <= INT32_MAX


def _emit_int(v: int) -> str:
    return "(-2147483647 - 1)" if v == INT32_MIN else (
        "(%d)" % v if v < 0 else str(v))


class Property:
    name = "base"

    def programs(self, rng: random.Random, count: int):
        """Yields (description, source, expected_lines) tuples."""
        raise NotImplementedError


class AddSubRoundTrip(Property):
    name = "x + y - y == x (overflow-screened)"

    def programs(self, rng, count):
        made = 0
        while made < count:
            x, y = _int_pool(rng, 2)
            if not (_fits32(x + y) and _fits32(x)):
                continue  # oracle screens overflow cases out
            made += 1
            src = ("main {\n    let x = %s\n    let y = %s\n"
                   "    if x + y - y == x { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % (_emit_int(x), _emit_int(y)))
            yield ("x=%d y=%d" % (x, y), src, ["OK"])


class AddCommutative(Property):
    name = "x + y == y + x"

    def programs(self, rng, count):
        made = 0
        while made < count:
            x, y = _int_pool(rng, 2)
            if not _fits32(x + y):
                continue
            made += 1
            src = ("main {\n    if %s + %s == %s + %s { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % (_emit_int(x), _emit_int(y), _emit_int(y), _emit_int(x)))
            yield ("x=%d y=%d" % (x, y), src, ["OK"])


class DoubleNegation(Property):
    name = "-(-x) == x"

    def programs(self, rng, count):
        made = 0
        while made < count:
            (x,) = _int_pool(rng, 1)
            if x == INT32_MIN:
                continue  # negation overflow — screened
            made += 1
            src = ("main {\n    let x = %s\n"
                   "    if -(-x) == x { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % _emit_int(x))
            yield ("x=%d" % x, src, ["OK"])


class ComparisonCoherence(Property):
    name = "!(a < b) == (a >= b) and trichotomy"

    def programs(self, rng, count):
        for _ in range(count):
            a, b = _int_pool(rng, 2)
            src = ("main {\n    let a = %s\n    let b = %s\n"
                   "    let p = !(a < b) == (a >= b)\n"
                   "    let lt = if a < b { 1 } else { 0 }\n"
                   "    let eq = if a == b { 1 } else { 0 }\n"
                   "    let gt = if a > b { 1 } else { 0 }\n"
                   "    if p && lt + eq + gt == 1 { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % (_emit_int(a), _emit_int(b)))
            yield ("a=%d b=%d" % (a, b), src, ["OK"])


class DeMorgan(Property):
    name = "!(p && q) == (!p || !q)"

    def programs(self, rng, count):
        for _ in range(count):
            p = "true" if rng.random() < 0.5 else "false"
            q = "true" if rng.random() < 0.5 else "false"
            src = ("main {\n    let p = %s\n    let q = %s\n"
                   "    if !(p && q) == (!p || !q) && !(p || q) == (!p && !q) "
                   "{ print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n" % (p, q))
            yield ("p=%s q=%s" % (p, q), src, ["OK"])


class NullCoalesce(Property):
    name = "x ?? y == x for non-null x"

    def programs(self, rng, count):
        for _ in range(count):
            (x,) = _int_pool(rng, 1)
            src = ("main {\n    let x = %s\n    let y = 42\n"
                   "    if (x ?? y) == x && (null ?? y) == y { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % _emit_int(x))
            yield ("x=%d" % x, src, ["OK"])


class ShortCircuitOrder(Property):
    name = "short-circuit evaluation-order parity (observed via prints)"

    def programs(self, rng, count):
        for i in range(count):
            # false && f() must not print RHS; true || f() must not print RHS
            src = ("fn side(x) {\n    print(\"side\" + x)\n    return true\n}\n\n"
                   "main {\n"
                   "    let a = false && side(1)\n"
                   "    let b = true || side(2)\n"
                   "    let c = true && side(3)\n"
                   "    let d = false || side(4)\n"
                   "    print(a)\n    print(b)\n    print(c)\n    print(d)\n}\n")
            yield ("fixed", src, ["side3", "side4", "false", "true",
                                  "true", "true"])
            return  # one deterministic case is exhaustive here


class ReverseInvolution(Property):
    name = "reverse(reverse(xs)) == id (via prints)"

    def programs(self, rng, count):
        for _ in range(count):
            xs = [rng.randint(-100, 100) for _ in range(rng.randint(0, 6))]
            lit = "[" + ", ".join(_emit_int(x) for x in xs) + "]"
            expect = "[" + ", ".join(str(x) for x in xs) + "]"
            src = ("use array\nmain {\n    let xs = %s\n"
                   "    print(array.reverse(array.reverse(xs)))\n}\n" % lit)
            yield ("xs=%r" % xs, src, [expect])


class SortedProperties(Property):
    name = "sorted: idempotent + non-decreasing + permutation"

    def programs(self, rng, count):
        for _ in range(count):
            xs = [rng.randint(-100, 100) for _ in range(rng.randint(0, 8))]
            lit = "[" + ", ".join(_emit_int(x) for x in xs) + "]"
            expect = "[" + ", ".join(str(x) for x in sorted(xs)) + "]"
            src = ("use array\nmain {\n    let xs = %s\n"
                   "    let s = array.sorted(xs)\n    print(s)\n"
                   "    print(array.sorted(s))\n}\n" % lit)
            yield ("xs=%r" % xs, src, [expect, expect])


class ConcatLength(Property):
    name = "length(a + b) == length(a) + length(b)"

    def programs(self, rng, count):
        pool = ["", "a", "xyz", "hello world", "NAAb", "  ", "0123456789"]
        for _ in range(count):
            a, b = rng.choice(pool), rng.choice(pool)
            src = ("use string\nmain {\n"
                   "    let a = \"%s\"\n    let b = \"%s\"\n"
                   "    if string.length(a + b) == string.length(a) + string.length(b) "
                   "{ print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n" % (a, b))
            yield ("a=%r b=%r" % (a, b), src, ["OK"])


class RepeatLength(Property):
    name = 'length(s * n) == n * length(s)'

    def programs(self, rng, count):
        pool = ["a", "ab", "xyz"]
        for _ in range(count):
            s, n = rng.choice(pool), rng.randint(0, 10)
            src = ("use string\nmain {\n    let s = \"%s\"\n    let n = %d\n"
                   "    if string.length(s * n) == n * string.length(s) "
                   "{ print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n" % (s, n))
            yield ("s=%r n=%d" % (s, n), src, ["OK"])


class DivExactRational(Property):
    name = "(a / b) * b ~= a  — checked exactly via Fraction before asserting"

    def programs(self, rng, count):
        made = 0
        while made < count:
            a = rng.randint(-1000, 1000)
            b = rng.choice([1, 2, 4, 5, 8, 10, 16, 20, 25, 32, 64, -2, -4])
            # exact track: only assert when a/b is exactly representable in
            # binary64 AND (a/b)*b reconstructs a exactly in IEEE
            frac = Fraction(a, b)
            ieee = a / b
            if Fraction(ieee) != frac or ieee * b != float(a):
                continue
            made += 1
            src = ("main {\n    let a = %s\n    let b = %s\n"
                   "    if (a / b) * b == a { print(\"OK\") } else { print(\"PROPERTY_VIOLATED\") }\n}\n"
                   % (_emit_int(a), _emit_int(b)))
            yield ("a=%d b=%d" % (a, b), src, ["OK"])


ALL_PROPERTIES = [
    AddSubRoundTrip(), AddCommutative(), DoubleNegation(),
    ComparisonCoherence(), DeMorgan(), NullCoalesce(), ShortCircuitOrder(),
    ReverseInvolution(), SortedProperties(), ConcatLength(), RepeatLength(),
    DivExactRational(),
]


def run_properties(naab_bin: str, seed: int, count: int, verbose=print):
    failures = 0
    total = 0
    for prop in ALL_PROPERTIES:
        rng = random.Random(seed)
        prop_fail = 0
        for desc, src, expected in prop.programs(rng, count):
            total += 1
            for tree_walk in (False, True):
                res = run_engine(naab_bin, src, tree_walk)
                engine = "tw" if tree_walk else "vm"
                if res.rc != 0 or res.out_lines != expected:
                    prop_fail += 1
                    failures += 1
                    verbose("  FAIL [%s] %s (%s): rc=%d got=%r want=%r err=%r"
                            % (prop.name, desc, engine, res.rc,
                               res.out_lines[:4], expected[:4],
                               res.error_text[:100]))
                    break
        status = "ok" if prop_fail == 0 else "FAIL(%d)" % prop_fail
        verbose("  property: %-58s %s" % (prop.name, status))
    return total, failures
