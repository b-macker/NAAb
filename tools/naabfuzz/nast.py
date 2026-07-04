"""Mini-AST for generated NAAb programs.

Deliberately covers only the deterministic subset the fuzzer emits and the
oracle can evaluate exactly: 32-bit checked ints, IEEE doubles, byte strings,
bools, null, homogeneous lists, functions with a non-recursive call graph,
and structured control flow. No dicts, no polyglot, no I/O, no randomness.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional


class NaabType(Enum):
    INT = "int"
    FLOAT = "float"
    STR = "string"
    BOOL = "bool"
    LIST_INT = "list_int"
    LIST_STR = "list_str"
    NULL = "null"


# ---------------------------------------------------------------- expressions

class Expr:
    pass


@dataclass
class IntLit(Expr):
    value: int  # must fit in int32


@dataclass
class FloatLit(Expr):
    value: float


@dataclass
class StrLit(Expr):
    value: str  # ASCII only


@dataclass
class BoolLit(Expr):
    value: bool


@dataclass
class NullLit(Expr):
    pass


@dataclass
class ListLit(Expr):
    items: List[Expr]


@dataclass
class Var(Expr):
    name: str


@dataclass
class Unary(Expr):
    op: str  # "-" | "!"
    operand: Expr


@dataclass
class Binary(Expr):
    op: str  # + - * / % == != < <= > >= && || ??
    left: Expr
    right: Expr


@dataclass
class Call(Expr):
    """Module function call (module empty = user-defined function)."""
    module: str
    name: str
    args: List[Expr]


@dataclass
class Index(Expr):
    base: Expr
    index: Expr


# ----------------------------------------------------------------- statements

class Stmt:
    pass


@dataclass
class Let(Stmt):
    name: str
    expr: Expr


@dataclass
class Assign(Stmt):
    name: str
    expr: Expr


@dataclass
class Print(Stmt):
    expr: Expr


@dataclass
class If(Stmt):
    cond: Expr
    then_body: List[Stmt]
    else_body: Optional[List[Stmt]] = None


@dataclass
class While(Stmt):
    cond: Expr
    body: List[Stmt]


@dataclass
class ForRange(Stmt):
    var: str
    start: Expr  # literal ints only (bounded)
    end: Expr
    inclusive: bool
    body: List[Stmt]


@dataclass
class Return(Stmt):
    expr: Expr


# ------------------------------------------------------------------- top level

@dataclass
class Param:
    name: str
    type: NaabType


@dataclass
class FnDecl:
    name: str
    params: List[Param]
    ret: NaabType
    body: List[Stmt]


@dataclass
class Program:
    uses: List[str] = field(default_factory=list)  # stdlib modules
    fns: List[FnDecl] = field(default_factory=list)
    main_body: List[Stmt] = field(default_factory=list)
