"""Oracle self-tests: hand-computed values, no engine binary required.

Run: python3 -m naabfuzz selftest   (or python3 -m naabfuzz.tests)
"""

import unittest

from .. import nast as A
from ..emitter import Emitter
from ..gen import generate
from ..oracle import (Oracle, OracleGap, NaabError, fmt_double, trunc_mod,
                      INT32_MIN, INT32_MAX, E_OVERFLOW, E_DIV0, E_MOD0,
                      E_TYPE)


def run_main(*stmts):
    prog = A.Program(uses=[], fns=[], main_body=list(stmts))
    return Oracle().run(prog)


def print_of(expr):
    return run_main(A.Print(expr))


class TestFormatting(unittest.TestCase):
    def test_g15_examples(self):
        # Hand-audited against build/naab-lang (both engines)
        self.assertEqual(fmt_double(0.1 + 0.2), "0.3")
        self.assertEqual(fmt_double(1.0), "1")
        self.assertEqual(fmt_double(3.5), "3.5")
        self.assertEqual(fmt_double(-0.0), "-0")
        self.assertEqual(fmt_double(1e20), "1e+20")
        self.assertEqual(fmt_double(1.0 / 3.0), "0.333333333333333")
        self.assertEqual(fmt_double(123456789.123456789), "123456789.123457")

    def test_spellings(self):
        self.assertEqual(print_of(A.BoolLit(True)).lines[0].text, "true")
        self.assertEqual(print_of(A.BoolLit(False)).lines[0].text, "false")
        self.assertEqual(print_of(A.NullLit()).lines[0].text, "null")

    def test_list_print_unquoted(self):
        r = print_of(A.ListLit([A.StrLit("a"), A.StrLit("b")]))
        self.assertEqual(r.lines[0].text, "[a, b]")


class TestIntArithmetic(unittest.TestCase):
    def test_overflow_add(self):
        r = print_of(A.Binary("+", A.IntLit(INT32_MAX), A.IntLit(1)))
        self.assertEqual(r.error, E_OVERFLOW)

    def test_neg_int_min_overflows(self):
        r = print_of(A.Unary("-", A.Binary("-", A.IntLit(-INT32_MAX),
                                           A.IntLit(1))))
        self.assertEqual(r.error, E_OVERFLOW)

    def test_mod_signs(self):
        self.assertEqual(trunc_mod(-7, 3), -1)
        self.assertEqual(trunc_mod(7, -3), 1)
        self.assertEqual(trunc_mod(-7, 2), -1)
        self.assertEqual(trunc_mod(7, -2), 1)
        self.assertEqual(trunc_mod(INT32_MIN, -1), 0)

    def test_mod_by_zero(self):
        r = print_of(A.Binary("%", A.IntLit(5), A.IntLit(0)))
        self.assertEqual(r.error, E_MOD0)

    def test_mod_float_is_type_error(self):
        r = print_of(A.Binary("%", A.FloatLit(1.5), A.IntLit(2)))
        self.assertEqual(r.error, E_TYPE)


class TestDivision(unittest.TestCase):
    def test_int_div_promotes(self):
        r = print_of(A.Binary("/", A.IntLit(7), A.IntLit(2)))
        self.assertIsNone(r.error)
        self.assertEqual(r.lines[0].text, "3.5")

    def test_exact_div_prints_intlike(self):
        r = print_of(A.Binary("/", A.IntLit(8), A.IntLit(2)))
        self.assertEqual(r.lines[0].text, "4")

    def test_int_min_div_minus_one(self):
        int_min = A.Binary("-", A.IntLit(-INT32_MAX), A.IntLit(1))
        r = print_of(A.Binary("/", int_min, A.IntLit(-1)))
        self.assertEqual(r.lines[0].text, "2147483648")

    def test_div_zero(self):
        r = print_of(A.Binary("/", A.IntLit(1), A.IntLit(0)))
        self.assertEqual(r.error, E_DIV0)

    def test_float_div_zero(self):
        r = print_of(A.Binary("/", A.FloatLit(1.5), A.FloatLit(0.0)))
        self.assertEqual(r.error, E_DIV0)


class TestStringsAndMath(unittest.TestCase):
    def test_concat_number_formatting(self):
        r = print_of(A.Binary("+", A.StrLit("x"), A.FloatLit(3.5)))
        self.assertEqual(r.lines[0].text, "x3.5")

    def test_abs_always_float(self):
        # math.abs(-3) % 2 must be a type error (abs returns float)
        r = print_of(A.Binary("%", A.Call("math", "abs", [A.IntLit(-3)]),
                              A.IntLit(2)))
        self.assertEqual(r.error, E_TYPE)

    def test_min_int_only_when_both_int(self):
        r = print_of(A.Binary("%", A.Call("math", "min",
                                          [A.IntLit(3), A.IntLit(5)]),
                              A.IntLit(2)))
        self.assertIsNone(r.error)
        self.assertEqual(r.lines[0].text, "1")
        r = print_of(A.Binary("%", A.Call("math", "min",
                                          [A.IntLit(3), A.FloatLit(5.0)]),
                              A.IntLit(2)))
        self.assertEqual(r.error, E_TYPE)

    def test_sqrt_is_approx(self):
        r = print_of(A.Call("math", "sqrt", [A.FloatLit(2.0)]))
        self.assertTrue(r.lines[0].approx)

    def test_round_half_away_from_zero(self):
        r = print_of(A.Call("math", "round", [A.FloatLit(2.5)]))
        self.assertEqual(r.lines[0].text, "3")
        r = print_of(A.Call("math", "round", [A.FloatLit(-2.5)]))
        self.assertEqual(r.lines[0].text, "-3")


class TestOracleGaps(unittest.TestCase):
    def test_list_eq_is_gap(self):
        with self.assertRaises(OracleGap):
            Oracle().run(A.Program(main_body=[
                A.Print(A.Binary("==",
                                 A.ListLit([A.IntLit(1)]),
                                 A.ListLit([A.IntLit(1)])))]))


class TestEmitterAndGen(unittest.TestCase):
    def test_seed_reproducibility(self):
        for seed in (0, 1, 42, 999):
            a = Emitter().program(generate(seed))
            b = Emitter().program(generate(seed))
            self.assertEqual(a, b, "seed %d not reproducible" % seed)

    def test_int_min_literal_rewritten(self):
        src = Emitter().expr(A.IntLit(INT32_MIN))
        self.assertEqual(src, "(-2147483647 - 1)")

    def test_no_exponent_floats(self):
        src = Emitter().expr(A.FloatLit(1e10))
        self.assertNotIn("e", src)
        self.assertEqual(float(src), 1e10)

    def test_precedence_roundtrip(self):
        # (1 + 2) * 3 must keep parens; 1 + (2 * 3) must not need them
        e1 = A.Binary("*", A.Binary("+", A.IntLit(1), A.IntLit(2)), A.IntLit(3))
        self.assertEqual(Emitter().expr(e1), "(1 + 2) * 3")
        e2 = A.Binary("+", A.IntLit(1), A.Binary("*", A.IntLit(2), A.IntLit(3)))
        self.assertEqual(Emitter().expr(e2), "1 + 2 * 3")

    def test_left_assoc_right_child_parenthesized(self):
        # 1 - (2 - 3) needs parens; (1 - 2) - 3 does not
        e = A.Binary("-", A.IntLit(1), A.Binary("-", A.IntLit(2), A.IntLit(3)))
        self.assertEqual(Emitter().expr(e), "1 - (2 - 3)")
        e = A.Binary("-", A.Binary("-", A.IntLit(1), A.IntLit(2)), A.IntLit(3))
        self.assertEqual(Emitter().expr(e), "1 - 2 - 3")

    def test_generated_programs_have_end_sentinel(self):
        for seed in range(5):
            src = Emitter().program(generate(seed))
            self.assertIn('print("END")', src)


if __name__ == "__main__":
    unittest.main()
