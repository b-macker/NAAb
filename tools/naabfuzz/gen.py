"""Grammar-based generator of deterministic NAAb programs.

Contract: the same seed always produces a byte-identical program (tested).
Every generated program terminates (literal-bounded loops), is free of
nondeterminism (no random/time/env/dict-order/non-ASCII), and stays inside
the subset the oracle models exactly.

Error paths are valuable: divisors are only 70% guaranteed-nonzero, overflow
is allowed — the oracle predicts the resulting error category.
"""

from __future__ import annotations

import random
from typing import Dict, List, Optional

from . import nast as A
from .nast import NaabType as T

MAX_DEPTH = 6
IDENT_POOL = ["a", "b", "c", "d", "e", "f", "g", "h", "k", "m",
              "n", "p", "q", "r", "s", "t", "u", "v", "w", "z"]
STR_POOL = ["", "a", "abc", "hello", "NAAb", "x y", "0", "-1", "zz top", "Zebra"]

INT32_MIN = -(2 ** 31)
INT32_MAX = 2 ** 31 - 1

_INT_EDGES = [0, 1, -1, 2, -2, 7, 10, 100, 255, 256, -100,
              32767, 65536, INT32_MAX, INT32_MIN, INT32_MAX - 1, INT32_MIN + 1]
# No exponent-notation values that could reach inf through * (NaN/inf
# printing is engine-verified but keeps v1 simpler to exclude)
_FLOAT_EDGES = [0.0, 1.0, -1.0, 0.5, -0.5, 3.5, 0.1, 2.0, 1e10, 0.0001,
                123456.789, -2.25, 99999.75]


class Scope:
    def __init__(self, parent: Optional["Scope"] = None):
        self.vars: Dict[str, T] = {}
        self.parent = parent

    def all_of(self, ty: T) -> List[str]:
        found = [] if self.parent is None else self.parent.all_of(ty)
        found.extend(n for n, t in self.vars.items() if t == ty)
        return found

    def names(self):
        s = set() if self.parent is None else self.parent.names()
        s.update(self.vars)
        return s


class Gen:
    def __init__(self, seed: int):
        self.rng = random.Random(seed)
        self.seed = seed
        self.fns: List[A.FnDecl] = []
        self.fn_sigs: List[tuple] = []  # (name, [param types], ret type)
        self._name_counter = 0

    # ------------------------------------------------------------- helpers

    def fresh(self, scope: Scope) -> str:
        taken = scope.names()
        for nm in IDENT_POOL:
            if nm not in taken:
                return nm
        self._name_counter += 1
        return "v%d" % self._name_counter

    def pick(self, seq):
        return self.rng.choice(seq)

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    # ------------------------------------------------------------ literals

    def int_lit(self) -> A.Expr:
        if self.chance(0.4):
            return A.IntLit(self.pick(_INT_EDGES))
        return A.IntLit(self.rng.randint(-1000, 1000))

    def float_lit(self) -> A.Expr:
        if self.chance(0.4):
            return A.FloatLit(self.pick(_FLOAT_EDGES))
        return A.FloatLit(round(self.rng.uniform(-1000, 1000), 6))

    def literal(self, ty: T) -> A.Expr:
        if ty == T.INT:
            return self.int_lit()
        if ty == T.FLOAT:
            return self.float_lit()
        if ty == T.STR:
            return A.StrLit(self.pick(STR_POOL))
        if ty == T.BOOL:
            return A.BoolLit(self.chance(0.5))
        if ty == T.LIST_INT:
            n = self.rng.randint(0, 5)
            return A.ListLit([self.int_lit() for _ in range(n)])
        if ty == T.LIST_STR:
            n = self.rng.randint(0, 4)
            return A.ListLit([A.StrLit(self.pick(STR_POOL)) for _ in range(n)])
        raise ValueError(ty)

    # ---------------------------------------------------------- expressions

    def expr(self, ty: T, depth: int, scope: Scope) -> A.Expr:
        if depth >= MAX_DEPTH:
            return self.leaf(ty, scope)
        r = self.rng.random()
        if r < 0.30:
            return self.leaf(ty, scope)
        if ty in (T.INT, T.FLOAT):
            return self.num_expr(ty, depth, scope)
        if ty == T.BOOL:
            return self.bool_expr(depth, scope)
        if ty == T.STR:
            return self.str_expr(depth, scope)
        if ty in (T.LIST_INT, T.LIST_STR):
            return self.leaf(ty, scope)
        return self.leaf(ty, scope)

    def leaf(self, ty: T, scope: Scope) -> A.Expr:
        cands = scope.all_of(ty)
        if cands and self.chance(0.55):
            return A.Var(self.pick(cands))
        return self.literal(ty)

    def num_expr(self, ty: T, depth: int, scope: Scope) -> A.Expr:
        d = depth + 1
        choices = ["binop", "binop", "binop", "unary", "call"]
        # user fn call producing this type
        fn_cands = [s for s in self.fn_sigs if s[2] == ty]
        if fn_cands:
            choices.append("userfn")
        kind = self.pick(choices)
        if kind == "unary":
            return A.Unary("-", self.expr(ty, d, scope))
        if kind == "userfn":
            name, ptys, _ = self.pick(fn_cands)
            return A.Call("", name, [self.expr(p, d, scope) for p in ptys])
        if kind == "call":
            return self.math_call(ty, d, scope)
        # binop
        if ty == T.INT:
            op = self.pick(["+", "-", "*", "%", "%"])
        else:
            op = self.pick(["+", "-", "*", "/", "/"])
        if op == "/":
            left = self.expr(self.pick([T.INT, T.FLOAT]), d, scope)
            right = self.denominator(d, scope)
            return A.Binary("/", left, right)
        if op == "%":
            left = self.expr(T.INT, d, scope)
            right = self.denominator(d, scope, int_only=True)
            return A.Binary("%", left, right)
        if ty == T.FLOAT:
            # At least one float operand; the other may be int (promotion)
            lt = self.pick([T.FLOAT, T.FLOAT, T.INT])
            rt = T.FLOAT if lt == T.INT else self.pick([T.FLOAT, T.INT])
            return A.Binary(op, self.expr(lt, d, scope), self.expr(rt, d, scope))
        return A.Binary(op, self.expr(T.INT, d, scope), self.expr(T.INT, d, scope))

    def denominator(self, depth: int, scope: Scope, int_only: bool = False) -> A.Expr:
        """70% guaranteed-nonzero literal, 30% arbitrary expression."""
        if self.chance(0.7):
            if int_only or self.chance(0.6):
                v = 0
                while v == 0:
                    v = self.rng.randint(-50, 50)
                return A.IntLit(v)
            v = 0.0
            while v == 0.0:
                v = round(self.rng.uniform(-50, 50), 3)
            return A.FloatLit(v)
        return self.expr(T.INT if int_only else self.pick([T.INT, T.FLOAT]),
                         depth, scope)

    def math_call(self, ty: T, depth: int, scope: Scope) -> A.Expr:
        if ty == T.INT:
            # NB: math.abs is NOT here — it always returns float
            name = self.pick(["min", "max", "floor", "ceil", "round"])
            if name in ("min", "max"):
                return A.Call("math", name,
                              [self.expr(T.INT, depth, scope),
                               self.expr(T.INT, depth, scope)])
            # floor/ceil/round: float arg, int result (bounded arg keeps it
            # inside int32 so the oracle needn't predict clamp overflow often)
            return A.Call("math", name, [self.expr(T.FLOAT, depth, scope)])
        # FLOAT result
        name = self.pick(["abs", "min", "max", "sqrt"])
        if name == "abs":
            return A.Call("math", "abs", [self.expr(T.FLOAT, depth, scope)])
        if name == "sqrt":
            return A.Call("math", "sqrt", [self.expr(T.FLOAT, depth, scope)])
        # min/max with at least one float
        return A.Call("math", name, [self.expr(T.FLOAT, depth, scope),
                                     self.expr(self.pick([T.INT, T.FLOAT]),
                                               depth, scope)])

    def bool_expr(self, depth: int, scope: Scope) -> A.Expr:
        d = depth + 1
        kind = self.pick(["cmp", "cmp", "cmp", "logic", "not", "eq"])
        if kind == "not":
            return A.Unary("!", self.expr(T.BOOL, d, scope))
        if kind == "logic":
            op = self.pick(["&&", "||"])
            return A.Binary(op, self.expr(T.BOOL, d, scope),
                            self.expr(T.BOOL, d, scope))
        if kind == "eq":
            op = self.pick(["==", "!="])
            fam = self.pick([T.INT, T.FLOAT, T.STR, T.BOOL])
            return A.Binary(op, self.expr(fam, d, scope), self.expr(fam, d, scope))
        op = self.pick(["<", "<=", ">", ">="])
        if self.chance(0.25):
            return A.Binary(op, self.expr(T.STR, d, scope), self.expr(T.STR, d, scope))
        lt = self.pick([T.INT, T.FLOAT])
        rt = self.pick([T.INT, T.FLOAT])
        return A.Binary(op, self.expr(lt, d, scope), self.expr(rt, d, scope))

    def str_expr(self, depth: int, scope: Scope) -> A.Expr:
        d = depth + 1
        kind = self.pick(["concat", "concat", "leaf", "concat_num"])
        if kind == "leaf":
            return self.leaf(T.STR, scope)
        if kind == "concat_num":
            num = self.expr(self.pick([T.INT, T.FLOAT, T.BOOL]), d, scope)
            return A.Binary("+", self.expr(T.STR, d, scope), num)
        return A.Binary("+", self.expr(T.STR, d, scope), self.expr(T.STR, d, scope))

    # ------------------------------------------------------------ statements

    def stmts(self, scope: Scope, depth: int, budget: int) -> List[A.Stmt]:
        out: List[A.Stmt] = []
        n = self.rng.randint(1, max(1, budget))
        for _ in range(n):
            out.extend(self.stmt(scope, depth))
        return out

    def stmt(self, scope: Scope, depth: int) -> List[A.Stmt]:
        r = self.rng.random()
        if r < 0.45 or depth >= 2:
            return self.let_and_maybe_print(scope, depth)
        if r < 0.60:
            cond = self.expr(T.BOOL, 0, scope)
            then_b = self.stmts(Scope(scope), depth + 1, 2)
            else_b = self.stmts(Scope(scope), depth + 1, 2) if self.chance(0.6) else None
            return [A.If(cond, then_b, else_b)]
        if r < 0.75:
            # bounded for
            var = self.fresh(scope)
            lo = self.rng.randint(-5, 5)
            hi = lo + self.rng.randint(0, 8)
            inner = Scope(scope)
            inner.vars[var] = T.INT
            body = self.stmts(inner, depth + 1, 2)
            return [A.ForRange(var, A.IntLit(lo), A.IntLit(hi),
                               self.chance(0.3), body)]
        if r < 0.85:
            # bounded while counter idiom
            i = self.fresh(scope)
            scope.vars[i] = T.INT
            n = self.rng.randint(1, 12)
            inner = Scope(scope)
            body = self.stmts(inner, depth + 1, 2)
            body.append(A.Assign(i, A.Binary("+", A.Var(i), A.IntLit(1))))
            return [A.Let(i, A.IntLit(0)),
                    A.While(A.Binary("<", A.Var(i), A.IntLit(n)), body)]
        return self.let_and_maybe_print(scope, depth)

    def let_and_maybe_print(self, scope: Scope, depth: int) -> List[A.Stmt]:
        ty = self.pick([T.INT, T.INT, T.FLOAT, T.FLOAT, T.STR, T.BOOL,
                        T.LIST_INT])
        name = self.fresh(scope)
        e = self.expr(ty, 0, scope)
        scope.vars[name] = ty
        out: List[A.Stmt] = [A.Let(name, e)]
        if self.chance(0.8):
            if ty in (T.LIST_INT, T.LIST_STR) and self.chance(0.5):
                out.append(A.Print(A.Call("array", "length", [A.Var(name)])))
            else:
                out.append(A.Print(A.Var(name)))
        return out

    # -------------------------------------------------------------- top level

    def gen_fn(self, idx: int) -> A.FnDecl:
        nparams = self.rng.randint(1, 3)
        ptys = [self.pick([T.INT, T.FLOAT]) for _ in range(nparams)]
        ret = self.pick([T.INT, T.FLOAT])
        name = "fn%d" % idx
        scope = Scope()
        params = []
        for i, pt in enumerate(ptys):
            pn = "p%d" % i
            params.append(A.Param(pn, pt))
            scope.vars[pn] = pt
        body = self.stmts(scope, 1, 2)
        body.append(A.Return(self.expr(ret, 0, scope)))
        return A.FnDecl(name, params, ret, body)

    def gen_program(self) -> A.Program:
        prog = A.Program(uses=["math", "array"])
        nfns = self.rng.randint(0, 3)
        for i in range(nfns):
            fn = self.gen_fn(i)
            prog.fns.append(fn)
            # register AFTER generating so fn bodies only call earlier fns
            self.fn_sigs.append((fn.name, [p.type for p in fn.params], fn.ret))
        scope = Scope()
        prog.main_body = self.stmts(scope, 0, 8)
        prog.main_body.append(A.Print(A.StrLit("END")))
        return prog


def generate(seed: int) -> A.Program:
    return Gen(seed).gen_program()
