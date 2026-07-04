"""nast -> NAAb source text.

The precedence table mirrors src/parser/parser.cpp exactly, including the
quirk that range (.. ..=) binds *between* equality and comparison:

    assignment < pipeline < ?? < or < and < equality < range < comparison
    < term (+ -) < factor (* / %) < unary < postfix

`paren_all=True` parenthesizes every subexpression; emitting the same AST
both ways and diffing engine output doubles as a free parser-precedence test.
"""

from __future__ import annotations

from . import nast as A

# Binding power of each binary operator (higher binds tighter).
# Mirrors parser.cpp's recursive-descent chain.
_PRECEDENCE = {
    "??": 3,
    "||": 4,
    "&&": 5,
    "==": 6, "!=": 6,
    # range .. / ..= would be 7 (between equality and comparison)
    "<": 8, "<=": 8, ">": 8, ">=": 8,
    "+": 9, "-": 9,
    "*": 10, "/": 10, "%": 10,
}
_UNARY_PREC = 11

# All emitted binary ops are left-associative in parser.cpp.


def _escape_str(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            raise ValueError("non-ASCII/control char in generated string: %r" % ch)
    return '"' + "".join(out) + '"'


def _fmt_float(v: float) -> str:
    if v != v or v in (float("inf"), float("-inf")):
        raise ValueError("cannot emit non-finite float literal")
    text = repr(v)
    if "e" in text or "E" in text:
        # NAAb has no scientific-notation literals (1e10 is a parse error);
        # expand to the exact decimal form of the binary64 value.
        from decimal import Decimal
        text = format(Decimal(v), "f")
    # Ensure it lexes as a float literal, not an int
    if "." not in text:
        text += ".0"
    return text


class Emitter:
    def __init__(self, paren_all: bool = False):
        self.paren_all = paren_all

    # -------------------------------------------------------------- programs

    def program(self, p: A.Program) -> str:
        lines = []
        for mod in p.uses:
            lines.append("use %s" % mod)
        if p.uses:
            lines.append("")
        for fn in p.fns:
            lines.extend(self._fn(fn))
            lines.append("")
        lines.append("main {")
        for st in p.main_body:
            lines.extend(self._stmt(st, 1))
        lines.append("}")
        return "\n".join(lines) + "\n"

    def _fn(self, fn: A.FnDecl):
        params = ", ".join(p.name for p in fn.params)
        lines = ["fn %s(%s) {" % (fn.name, params)]
        for st in fn.body:
            lines.extend(self._stmt(st, 1))
        lines.append("}")
        return lines

    # ------------------------------------------------------------ statements

    def _stmt(self, st: A.Stmt, depth: int):
        ind = "    " * depth
        if isinstance(st, A.Let):
            return [ind + "let %s = %s" % (st.name, self.expr(st.expr))]
        if isinstance(st, A.Assign):
            return [ind + "%s = %s" % (st.name, self.expr(st.expr))]
        if isinstance(st, A.Print):
            return [ind + "print(%s)" % self.expr(st.expr)]
        if isinstance(st, A.Return):
            return [ind + "return %s" % self.expr(st.expr)]
        if isinstance(st, A.If):
            lines = [ind + "if %s {" % self.expr(st.cond)]
            for s in st.then_body:
                lines.extend(self._stmt(s, depth + 1))
            if st.else_body is not None:
                lines.append(ind + "} else {")
                for s in st.else_body:
                    lines.extend(self._stmt(s, depth + 1))
            lines.append(ind + "}")
            return lines
        if isinstance(st, A.While):
            lines = [ind + "while %s {" % self.expr(st.cond)]
            for s in st.body:
                lines.extend(self._stmt(s, depth + 1))
            lines.append(ind + "}")
            return lines
        if isinstance(st, A.ForRange):
            op = "..=" if st.inclusive else ".."
            lines = [ind + "for %s in %s%s%s {" % (
                st.var, self.expr(st.start), op, self.expr(st.end))]
            for s in st.body:
                lines.extend(self._stmt(s, depth + 1))
            lines.append(ind + "}")
            return lines
        raise TypeError("unknown stmt %r" % st)

    # ----------------------------------------------------------- expressions

    def expr(self, e: A.Expr) -> str:
        return self._expr(e, 0)

    def _expr(self, e: A.Expr, parent_prec: int) -> str:
        if isinstance(e, A.IntLit):
            if e.value == -(2 ** 31):
                # The literal -2147483648 lexes as a FLOAT (2147483648
                # overflows int32 before unary minus applies); build INT_MIN
                # arithmetically instead.
                return "(-2147483647 - 1)"
            # Negative literals need parens in operand position: a * -3
            text = str(e.value)
            if e.value < 0 and parent_prec > 0:
                return "(%s)" % text
            return text
        if isinstance(e, A.FloatLit):
            text = _fmt_float(e.value)
            if e.value < 0 and parent_prec > 0:
                return "(%s)" % text
            return text
        if isinstance(e, A.StrLit):
            return _escape_str(e.value)
        if isinstance(e, A.BoolLit):
            return "true" if e.value else "false"
        if isinstance(e, A.NullLit):
            return "null"
        if isinstance(e, A.ListLit):
            return "[%s]" % ", ".join(self._expr(i, 0) for i in e.items)
        if isinstance(e, A.Var):
            return e.name
        if isinstance(e, A.Call):
            args = ", ".join(self._expr(a, 0) for a in e.args)
            if e.module:
                return "%s.%s(%s)" % (e.module, e.name, args)
            return "%s(%s)" % (e.name, args)
        if isinstance(e, A.Index):
            return "%s[%s]" % (self._expr(e.base, _UNARY_PREC + 1),
                               self._expr(e.index, 0))
        if isinstance(e, A.Unary):
            inner = self._expr(e.operand, _UNARY_PREC)
            text = e.op + inner
            if self.paren_all:
                return "(%s)" % text
            if parent_prec > _UNARY_PREC:
                return "(%s)" % text
            return text
        if isinstance(e, A.Binary):
            prec = _PRECEDENCE[e.op]
            # Left-assoc: left child may share this level; right child must
            # bind tighter to reproduce the same tree.
            lhs = self._expr(e.left, prec)
            rhs = self._expr(e.right, prec + 1)
            text = "%s %s %s" % (lhs, e.op, rhs)
            if self.paren_all:
                return "(%s)" % text
            if parent_prec > prec:
                return "(%s)" % text
            return text
        raise TypeError("unknown expr %r" % e)
