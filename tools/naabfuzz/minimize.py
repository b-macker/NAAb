"""AST-level minimization of fuzzer findings.

Shrinks at the AST level and re-emits, so every candidate stays syntactically
valid by construction. Passes (repeated until fixpoint, bounded by
MAX_EXECUTIONS engine runs):

  1. ddmin-style removal of top-level fns and statements (with best-effort
     dependency safety: a candidate that introduces unbound vars just fails
     the predicate and is rejected)
  2. replace Let RHS subexpressions with their oracle-computed literal value
  3. shrink int literals toward 0, strings toward "", lists toward []
  4. unwrap if/while/for bodies into the enclosing block

The predicate is "triage signature unchanged".
"""

from __future__ import annotations

import copy
from typing import List, Optional

from . import nast as A
from .emitter import Emitter
from .oracle import Oracle, OracleGap, NaabError, OracleResult, NV
from .runner import run_both
from .triage import triage, Finding

MAX_EXECUTIONS = 200


class Minimizer:
    def __init__(self, naab_bin: str, target: Finding, known_detectors=None):
        self.naab_bin = naab_bin
        self.target = target
        self.known_detectors = known_detectors or []
        self.executions = 0
        self.emitter = Emitter()

    def oracle_for(self, prog: A.Program) -> Optional[OracleResult]:
        try:
            return Oracle().run(prog)
        except OracleGap:
            return None

    def still_fails(self, prog: A.Program) -> bool:
        if self.executions >= MAX_EXECUTIONS:
            return False
        self.executions += 1
        try:
            src = self.emitter.program(prog)
        except (ValueError, TypeError):
            return False
        oracle = self.oracle_for(prog)
        if oracle is None:
            oracle = OracleResult([], None)  # differential-only fallback
        vm, tw = run_both(self.naab_bin, src)
        f = triage(vm, tw, oracle, self.known_detectors)
        return (f.classification == self.target.classification
                and f.signature == self.target.signature)

    # ------------------------------------------------------------- passes

    def minimize(self, prog: A.Program) -> A.Program:
        prog = copy.deepcopy(prog)
        changed = True
        while changed and self.executions < MAX_EXECUTIONS:
            changed = False
            changed |= self.shrink_list_field(prog, "fns")
            changed |= self.shrink_stmts(prog.main_body, prog)
            changed |= self.unwrap_blocks(prog.main_body, prog)
            changed |= self.shrink_literals(prog)
        return prog

    def shrink_list_field(self, prog: A.Program, field: str) -> bool:
        items = getattr(prog, field)
        changed = False
        i = 0
        while i < len(items):
            trial = items[:i] + items[i + 1:]
            setattr(prog, field, trial)
            if self.still_fails(prog):
                items = trial
                changed = True
            else:
                setattr(prog, field, items)
                i += 1
        return changed

    def shrink_stmts(self, body: List[A.Stmt], prog: A.Program) -> bool:
        changed = False
        i = 0
        while i < len(body):
            removed = body.pop(i)
            if self.still_fails(prog):
                changed = True
            else:
                body.insert(i, removed)
                # recurse into nested bodies
                st = body[i]
                for sub in self.sub_bodies(st):
                    changed |= self.shrink_stmts(sub, prog)
                i += 1
        return changed

    @staticmethod
    def sub_bodies(st: A.Stmt):
        if isinstance(st, A.If):
            yield st.then_body
            if st.else_body is not None:
                yield st.else_body
        elif isinstance(st, (A.While, A.ForRange)):
            yield st.body

    def unwrap_blocks(self, body: List[A.Stmt], prog: A.Program) -> bool:
        changed = False
        i = 0
        while i < len(body):
            st = body[i]
            if isinstance(st, A.If):
                saved = body[:]
                body[i:i + 1] = st.then_body
                if self.still_fails(prog):
                    changed = True
                    continue
                body[:] = saved
            i += 1
        return changed

    def shrink_literals(self, prog: A.Program) -> bool:
        changed = False
        for holder, attr, lit in self.iter_literals(prog):
            candidates = []
            if isinstance(lit, A.IntLit) and lit.value != 0:
                candidates = [A.IntLit(0), A.IntLit(1),
                              A.IntLit(lit.value // 2)]
            elif isinstance(lit, A.FloatLit) and lit.value != 0.0:
                candidates = [A.FloatLit(0.0), A.FloatLit(1.0)]
            elif isinstance(lit, A.StrLit) and lit.value:
                candidates = [A.StrLit(""), A.StrLit(lit.value[:1])]
            elif isinstance(lit, A.ListLit) and lit.items:
                candidates = [A.ListLit([])]
            for cand in candidates:
                if self.executions >= MAX_EXECUTIONS:
                    return changed
                old = getattr(holder, attr)
                setattr(holder, attr, cand)
                if self.still_fails(prog):
                    changed = True
                    break
                setattr(holder, attr, old)
        return changed

    def iter_literals(self, prog: A.Program):
        """Yields (holder, attr, literal) for every literal in the program."""
        def walk_expr(holder, attr, e):
            if isinstance(e, (A.IntLit, A.FloatLit, A.StrLit, A.ListLit)):
                yield holder, attr, e
            if isinstance(e, A.Unary):
                yield from walk_expr(e, "operand", e.operand)
            elif isinstance(e, A.Binary):
                yield from walk_expr(e, "left", e.left)
                yield from walk_expr(e, "right", e.right)
            elif isinstance(e, A.Call):
                for i in range(len(e.args)):
                    yield from walk_expr_list(e.args, i)
            elif isinstance(e, A.Index):
                yield from walk_expr(e, "base", e.base)
                yield from walk_expr(e, "index", e.index)

        def walk_expr_list(lst, i):
            e = lst[i]
            if isinstance(e, (A.IntLit, A.FloatLit, A.StrLit, A.ListLit)):
                yield _ListSlot(lst, i), "value", e
            else:
                yield from walk_expr(_NullHolder(), "x", e)

        def walk_stmt(st):
            if isinstance(st, (A.Let, A.Assign, A.Print, A.Return)):
                yield from walk_expr(st, "expr", st.expr)
            elif isinstance(st, A.If):
                yield from walk_expr(st, "cond", st.cond)
                for s in st.then_body:
                    yield from walk_stmt(s)
                for s in (st.else_body or []):
                    yield from walk_stmt(s)
            elif isinstance(st, A.While):
                yield from walk_expr(st, "cond", st.cond)
                for s in st.body:
                    yield from walk_stmt(s)
            elif isinstance(st, A.ForRange):
                for s in st.body:
                    yield from walk_stmt(s)

        for fn in prog.fns:
            for st in fn.body:
                yield from walk_stmt(st)
        for st in prog.main_body:
            yield from walk_stmt(st)


class _NullHolder:
    x = None


class _ListSlot:
    """Adapter so list elements can be set via setattr(holder, 'value', v)."""

    def __init__(self, lst, i):
        object.__setattr__(self, "_lst", lst)
        object.__setattr__(self, "_i", i)

    def __setattr__(self, name, v):
        self._lst[self._i] = v

    def __getattr__(self, name):
        return self._lst[self._i]
