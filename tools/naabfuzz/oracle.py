"""Exact-arithmetic oracle: an executable specification for the deterministic
NAAb subset emitted by the generator.

Independently evaluates a nast.Program and predicts stdout + error category,
so engine divergences can be attributed (VM bug vs tree-walker bug) instead
of merely detected.

Numeric model (verified against src/vm/vm.cpp + src/interpreter/expressions.cpp):
  - ints are 32-bit with checked overflow on + - * and unary - (throws
    "Integer overflow"); Python ints + chk32() reproduce this exactly.
  - `/` ALWAYS produces an IEEE double (post-unification semantics; both
    engines). Python floats are IEEE binary64, so + - * / on doubles are
    bit-exact with C++.
  - `%` is int-only, truncated toward zero; INT_MIN % -1 == 0; float operand
    is a type error; zero divisor is "Modulo by zero".
  - anything routed through libm (sqrt, pow beyond exact cases) is marked
    approx and compared within 4 ULP instead of exact string match.

Formatting model (empirically audited, see FORMAT AUDIT below):
  - doubles print via '%.15g'
  - true/false, null literal spellings
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from . import nast as A

INT32_MIN = -(2 ** 31)
INT32_MAX = 2 ** 31 - 1

# ------------------------------------------------------------- error model


class NaabError(Exception):
    """Predicted runtime error. `category` is a normalized token that the
    triage layer also extracts from engine output."""

    def __init__(self, category: str, note: str = ""):
        super().__init__("%s %s" % (category, note))
        self.category = category


class OracleGap(Exception):
    """The oracle cannot model this construct — a bug in the generator or a
    missing oracle feature, never a language bug."""


# Error categories (normalized tokens shared with triage.classify_error)
E_OVERFLOW = "integer_overflow"
E_DIV0 = "division_by_zero"
E_MOD0 = "modulo_by_zero"
E_TYPE = "type_error"
E_INDEX = "index_error"
E_DOMAIN = "domain_error"


# ------------------------------------------------------------ value model


@dataclass
class NV:
    """Oracle value: python payload + approx taint (touched libm)."""
    v: object  # int | float | str | bool | None | list[NV]
    approx: bool = False


@dataclass
class OutLine:
    text: str
    approx: bool = False  # compare via 4-ULP float parse, not exact string


@dataclass
class OracleResult:
    lines: List[OutLine] = field(default_factory=list)
    error: Optional[str] = None  # normalized category; expected exit code 1


# ---------------------------------------------------------- FORMAT AUDIT
# Empirical spellings, verified against build/naab-lang (both engines):
#   print(true)        -> true
#   print(false)       -> false
#   print(null)        -> null
#   print(1.0)         -> 1            ('%.15g')
#   print(3.5)         -> 3.5
#   print(0.1 + 0.2)   -> 0.3          ('%.15g' hides the ulp)
#   print([1, 2])      -> [1, 2]
#   "x" + 3.5          -> x3.5         (same '%.15g')
#   "x" + 1            -> x1

def fmt_double(d: float) -> str:
    if d != d:
        return "nan"
    if d == float("inf"):
        return "inf"
    if d == float("-inf"):
        return "-inf"
    return "%.15g" % d


def fmt(nv: NV) -> str:
    v = nv.v
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return fmt_double(v)
    if isinstance(v, str):
        return v
    if isinstance(v, list):
        return "[" + ", ".join(fmt_list_elem(x) for x in v) + "]"
    raise OracleGap("cannot format %r" % (v,))


def fmt_list_elem(nv: NV) -> str:
    # Strings inside printed lists are NOT quoted: print(["a","b"]) -> [a, b]
    return fmt(nv)


# -------------------------------------------------------------- arithmetic


def chk32(x: int, what: str) -> int:
    if x < INT32_MIN or x > INT32_MAX:
        raise NaabError(E_OVERFLOW, what)
    return x


def is_num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def trunc_mod(a: int, b: int) -> int:
    if b == -1:
        return 0  # INT_MIN % -1 guard; correct for all a
    r = abs(a) % abs(b)
    return -r if a < 0 else r


class _Return(Exception):
    def __init__(self, value: NV):
        self.value = value


class Oracle:
    STEP_BUDGET = 2_000_000

    def __init__(self):
        self.steps = 0
        self.fns: Dict[str, A.FnDecl] = {}
        self.out: List[OutLine] = []

    # ------------------------------------------------------------- driver

    def run(self, prog: A.Program) -> OracleResult:
        self.fns = {f.name: f for f in prog.fns}
        self.out = []
        try:
            env: Dict[str, NV] = {}
            for st in prog.main_body:
                self.exec_stmt(st, env)
        except NaabError as e:
            return OracleResult(self.out, e.category)
        return OracleResult(self.out, None)

    def tick(self):
        self.steps += 1
        if self.steps > self.STEP_BUDGET:
            raise OracleGap("step budget exceeded — generator emitted an unbounded loop?")

    # ---------------------------------------------------------- statements

    def exec_stmt(self, st: A.Stmt, env: Dict[str, NV]):
        self.tick()
        if isinstance(st, A.Let) or isinstance(st, A.Assign):
            env[st.name] = self.eval(st.expr, env)
        elif isinstance(st, A.Print):
            nv = self.eval(st.expr, env)
            self.out.append(OutLine(fmt(nv), approx=nv.approx))
        elif isinstance(st, A.Return):
            raise _Return(self.eval(st.expr, env))
        elif isinstance(st, A.If):
            c = self.eval(st.cond, env)
            if not isinstance(c.v, bool):
                raise OracleGap("non-bool if condition")
            if c.v:
                for s in st.then_body:
                    self.exec_stmt(s, env)
            elif st.else_body is not None:
                for s in st.else_body:
                    self.exec_stmt(s, env)
        elif isinstance(st, A.While):
            while True:
                self.tick()
                c = self.eval(st.cond, env)
                if not isinstance(c.v, bool):
                    raise OracleGap("non-bool while condition")
                if not c.v:
                    break
                for s in st.body:
                    self.exec_stmt(s, env)
        elif isinstance(st, A.ForRange):
            start = self.eval(st.start, env)
            end = self.eval(st.end, env)
            if not isinstance(start.v, int) or not isinstance(end.v, int):
                raise OracleGap("non-int for range")
            stop = end.v + 1 if st.inclusive else end.v
            for i in range(start.v, stop):
                self.tick()
                env[st.var] = NV(i)
                for s in st.body:
                    self.exec_stmt(s, env)
        else:
            raise OracleGap("unknown stmt %r" % st)

    # --------------------------------------------------------------- calls

    def call_fn(self, fn: A.FnDecl, args: List[NV]) -> NV:
        if len(args) != len(fn.params):
            raise OracleGap("arity mismatch calling %s" % fn.name)
        env = {p.name: a for p, a in zip(fn.params, args)}
        try:
            for st in fn.body:
                self.exec_stmt(st, env)
        except _Return as r:
            return r.value
        return NV(None)

    # ---------------------------------------------------------- expressions

    def eval(self, e: A.Expr, env: Dict[str, NV]) -> NV:
        self.tick()
        if isinstance(e, A.IntLit):
            return NV(e.value)
        if isinstance(e, A.FloatLit):
            return NV(e.value)
        if isinstance(e, A.StrLit):
            return NV(e.value)
        if isinstance(e, A.BoolLit):
            return NV(e.value)
        if isinstance(e, A.NullLit):
            return NV(None)
        if isinstance(e, A.ListLit):
            return NV([self.eval(i, env) for i in e.items])
        if isinstance(e, A.Var):
            if e.name not in env:
                raise OracleGap("unbound var %s" % e.name)
            return env[e.name]
        if isinstance(e, A.Unary):
            return self.eval_unary(e, env)
        if isinstance(e, A.Binary):
            return self.eval_binary(e, env)
        if isinstance(e, A.Index):
            base = self.eval(e.base, env)
            idx = self.eval(e.index, env)
            if not isinstance(base.v, list) or not isinstance(idx.v, int) \
                    or isinstance(idx.v, bool):
                raise OracleGap("index on non-list / non-int index")
            i = idx.v
            if i < 0 or i >= len(base.v):
                raise NaabError(E_INDEX, "list index out of range")
            return base.v[i]
        if isinstance(e, A.Call):
            args = [self.eval(a, env) for a in e.args]
            if e.module == "math":
                return self.call_math(e.name, args)
            if e.module == "string":
                return self.call_string(e.name, args)
            if e.module == "array":
                return self.call_array(e.name, args)
            if e.module == "":
                fn = self.fns.get(e.name)
                if fn is None:
                    raise OracleGap("unknown fn %s" % e.name)
                return self.call_fn(fn, args)
            raise OracleGap("unknown module %s" % e.module)
        raise OracleGap("unknown expr %r" % e)

    def eval_unary(self, e: A.Unary, env) -> NV:
        a = self.eval(e.operand, env)
        if e.op == "-":
            if isinstance(a.v, bool) or a.v is None or isinstance(a.v, str):
                raise NaabError(E_TYPE, "negate non-numeric")
            if isinstance(a.v, int):
                return NV(chk32(-a.v, "negation"), a.approx)
            return NV(-a.v, a.approx)
        if e.op == "!":
            if not isinstance(a.v, bool):
                raise OracleGap("! on non-bool")
            return NV(not a.v)
        raise OracleGap("unary %s" % e.op)

    def eval_binary(self, e: A.Binary, env) -> NV:
        op = e.op
        # Short-circuit ops evaluate left first
        if op in ("&&", "||"):
            l = self.eval(e.left, env)
            if not isinstance(l.v, bool):
                raise OracleGap("logical op on non-bool")
            if op == "&&" and not l.v:
                return NV(False)
            if op == "||" and l.v:
                return NV(True)
            r = self.eval(e.right, env)
            if not isinstance(r.v, bool):
                raise OracleGap("logical op on non-bool")
            return NV(r.v)
        if op == "??":
            l = self.eval(e.left, env)
            if l.v is not None:
                return l
            return self.eval(e.right, env)

        l = self.eval(e.left, env)
        r = self.eval(e.right, env)
        ap = l.approx or r.approx

        if op == "+":
            # String concat wins if either side is a string
            if isinstance(l.v, str) or isinstance(r.v, str):
                return NV(fmt(l) + fmt(r), ap)
            if isinstance(l.v, list) and isinstance(r.v, list):
                return NV(l.v + r.v, ap)
            self.require_num(l, r, "+")
            if isinstance(l.v, int) and isinstance(r.v, int):
                return NV(chk32(l.v + r.v, "addition"), ap)
            return NV(float(l.v) + float(r.v), ap)
        if op == "-":
            self.require_num(l, r, "-")
            if isinstance(l.v, int) and isinstance(r.v, int):
                return NV(chk32(l.v - r.v, "subtraction"), ap)
            return NV(float(l.v) - float(r.v), ap)
        if op == "*":
            self.require_num(l, r, "*")
            if isinstance(l.v, int) and isinstance(r.v, int):
                return NV(chk32(l.v * r.v, "multiplication"), ap)
            return NV(float(l.v) * float(r.v), ap)
        if op == "/":
            self.require_num(l, r, "/")
            if float(r.v) == 0.0:
                raise NaabError(E_DIV0)
            return NV(float(l.v) / float(r.v), ap)
        if op == "%":
            if not (isinstance(l.v, int) and not isinstance(l.v, bool) and
                    isinstance(r.v, int) and not isinstance(r.v, bool)):
                raise NaabError(E_TYPE, "modulo requires ints")
            if r.v == 0:
                raise NaabError(E_MOD0)
            return NV(trunc_mod(l.v, r.v), ap)

        if op in ("==", "!="):
            res = self.value_eq(l, r)
            return NV(res if op == "==" else not res, ap)
        if op in ("<", "<=", ">", ">="):
            if isinstance(l.v, str) and isinstance(r.v, str):
                a, b = l.v, r.v
            elif is_num(l.v) and is_num(r.v):
                a, b = l.v, r.v
            else:
                raise NaabError(E_TYPE, "comparison type mismatch")
            res = {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]
            return NV(res, ap)
        raise OracleGap("binary %s" % op)

    @staticmethod
    def require_num(l: NV, r: NV, op: str):
        if not (is_num(l.v) and is_num(r.v)):
            raise NaabError(E_TYPE, "op %s on non-numeric" % op)

    def value_eq(self, l: NV, r: NV) -> bool:
        if is_num(l.v) and is_num(r.v):
            return float(l.v) == float(r.v)
        if isinstance(l.v, str) and isinstance(r.v, str):
            return l.v == r.v
        if isinstance(l.v, bool) and isinstance(r.v, bool):
            return l.v == r.v
        if l.v is None or r.v is None:
            return l.v is None and r.v is None
        if isinstance(l.v, list) and isinstance(r.v, list):
            # Engines use reference equality for lists ([1,2]==[1,2] is
            # false) — not exactly predictable at AST level
            raise OracleGap("list == is reference equality")
        # Mixed families compare unequal without error ("a" == 1 -> false)
        return False

    # ------------------------------------------------------------- stdlib

    def call_math(self, name: str, args: List[NV]) -> NV:
        ap = any(a.approx for a in args)
        vs = [a.v for a in args]
        if name == "abs" and len(vs) == 1 and is_num(vs[0]):
            # math.abs ALWAYS returns float, even for int args (verified:
            # math.abs(-3) % 2 is a type error in both engines)
            return NV(abs(float(vs[0])), ap)
        if name in ("min", "max") and len(vs) == 2 and all(is_num(v) for v in vs):
            # Result is int only when BOTH args are ints (verified empirically)
            pick = min(vs) if name == "min" else max(vs)
            if isinstance(vs[0], int) and isinstance(vs[1], int):
                return NV(int(pick), ap)
            return NV(float(pick), ap)
        if name in ("floor", "ceil", "round") and len(vs) == 1 and is_num(vs[0]):
            x = float(vs[0])
            if name == "floor":
                r = math.floor(x)
            elif name == "ceil":
                r = math.ceil(x)
            else:
                # half away from zero
                r = math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5)
            # floor/ceil/round return int (verified: floor(3.7) % 2 works).
            # Out-of-int32 results WRAP via C cast in both engines
            # (floor(4e9) == -2147483648) — not worth modeling exactly.
            if r < INT32_MIN or r > INT32_MAX:
                raise OracleGap("%s result outside int32 wraps" % name)
            return NV(int(r), ap)
        if name == "sqrt" and len(vs) == 1 and is_num(vs[0]):
            if vs[0] < 0:
                # "sqrt() requires non-negative argument" in both engines
                raise NaabError(E_DOMAIN, "sqrt of negative")
            return NV(math.sqrt(float(vs[0])), True)  # libm — approx
        if name == "pow" and len(vs) == 2 and all(is_num(v) for v in vs):
            try:
                r = math.pow(float(vs[0]), float(vs[1]))
            except (OverflowError, ValueError):
                raise OracleGap("pow domain")
            return NV(r, True)  # libm — approx
        raise OracleGap("math.%s/%d" % (name, len(vs)))

    def call_string(self, name: str, args: List[NV]) -> NV:
        vs = [a.v for a in args]
        if name == "length" and len(vs) == 1 and isinstance(vs[0], str):
            return NV(len(vs[0]))  # ASCII-only, so bytes == chars
        if name == "upper" and len(vs) == 1 and isinstance(vs[0], str):
            return NV(vs[0].upper())
        if name == "lower" and len(vs) == 1 and isinstance(vs[0], str):
            return NV(vs[0].lower())
        raise OracleGap("string.%s/%d" % (name, len(vs)))

    def call_array(self, name: str, args: List[NV]) -> NV:
        vs = [a.v for a in args]
        if name == "length" and len(vs) == 1 and isinstance(vs[0], list):
            return NV(len(vs[0]))
        if name == "reverse" and len(vs) == 1 and isinstance(vs[0], list):
            return NV(list(reversed(vs[0])))
        if name == "sorted" and len(vs) == 1 and isinstance(vs[0], list):
            # Exact prediction only for same-type default-comparator lists;
            # the generator guarantees homogeneous int or string lists.
            items = vs[0]
            kinds = {type(x.v) for x in items}
            if len(kinds) > 1:
                raise OracleGap("mixed-type sort not exactly predictable")
            return NV(sorted(items, key=lambda nv: nv.v))
        raise OracleGap("array.%s/%d" % (name, len(vs)))


def eval_program(prog: A.Program) -> OracleResult:
    return Oracle().run(prog)
