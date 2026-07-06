"""AST node definitions for the cmm language.

Plain dataclasses. Every node carries (line, col) for diagnostics, and most
expression nodes get an inferred `type` attribute filled in by the analyzer.
"""

from dataclasses import dataclass, field
from typing import List, Optional


# ---- Top level -------------------------------------------------------------

@dataclass
class Module:
    classname: str
    is_native: bool
    uses: List["Use"]
    variables: List["ClassVar"]
    functions: List["Function"]
    line: int = 0
    col: int = 0
    # filled by analyzer
    var_index: dict = field(default_factory=dict)


@dataclass
class Use:
    # Exactly one of (name, url) is set.
    name: Optional[str]
    url: Optional[str]
    line: int = 0
    col: int = 0


@dataclass
class ClassVar:
    name: str
    type: "TypeRef"
    line: int = 0
    col: int = 0


@dataclass
class TypeRef:
    name: str                       # Int, String, List, Dict, Data, or a class
    args: List["TypeRef"] = field(default_factory=list)
    line: int = 0
    col: int = 0


@dataclass
class Param:
    name: str
    type: "TypeRef"
    line: int = 0
    col: int = 0


@dataclass
class Function:
    name: str
    params: List[Param]
    ret: Optional[TypeRef]          # None means no return value
    body: List["Stmt"]
    is_native: bool = False
    line: int = 0
    col: int = 0


# ---- Statements ------------------------------------------------------------

@dataclass
class Assign:
    """name = expr;  /  @name = expr;  /  target[index] = expr;"""
    target: "Expr"                  # Name, ClassVarRef, or Index
    value: "Expr"
    declares: bool = False          # set by analyzer if this is first assignment
    line: int = 0
    col: int = 0


@dataclass
class EmptyDecl:
    """name: Type;  /  @name handled separately."""
    name: str
    type: TypeRef
    line: int = 0
    col: int = 0


@dataclass
class IncDec:
    target: "Expr"                  # Name or ClassVarRef
    op: str                         # '++' or '--'
    line: int = 0
    col: int = 0


@dataclass
class ExprStmt:
    expr: "Expr"
    line: int = 0
    col: int = 0


@dataclass
class Return:
    value: Optional["Expr"]         # spec: must be a single variable (Name/ClassVarRef)
    line: int = 0
    col: int = 0


@dataclass
class If:
    cond: "Expr"
    then: List["Stmt"]
    otherwise: Optional[List["Stmt"]]
    line: int = 0
    col: int = 0


@dataclass
class While:
    cond: "Expr"
    body: List["Stmt"]
    line: int = 0
    col: int = 0


@dataclass
class For:
    var: str
    iterable: "Expr"
    body: List["Stmt"]
    line: int = 0
    col: int = 0
    # filled by analyzer
    elem_type: Optional[TypeRef] = None


@dataclass
class UseLock:
    vars: List[str]                 # class variable names (without @)
    body: List["Stmt"]
    line: int = 0
    col: int = 0


# ---- Expressions -----------------------------------------------------------

@dataclass
class IntLit:
    value: int
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class FloatLit:
    value: float
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class StringLit:
    value: str
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class BoolLit:
    value: bool
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class ListLit:
    items: List["Expr"]
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class DictLit:
    keys: List["Expr"]
    values: List["Expr"]
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class Name:
    """A local variable reference, OR a bare class name used as a namespace
    (e.g. Console, Math, User in User())."""
    name: str
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class ClassVarRef:
    """@name"""
    name: str
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class Binary:
    op: str
    left: "Expr"
    right: "Expr"
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class Unary:
    op: str                         # 'not' or '-'
    operand: "Expr"
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class EmptyCheck:
    operand: "Expr"
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class Index:
    target: "Expr"
    index: "Expr"
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class Call:
    """Method or constructor or namespaced call.

    receiver is None for a bare call (rare); for `Console.print(x)` the
    receiver is Name('Console') and method='print'; for `obj.foo()` receiver
    is the obj expression and method='foo'; for `User()` receiver is None,
    method=None and callee=Name('User')."""
    callee: "Expr"                  # the thing before '(' : Name or Member
    args: List["Expr"]
    line: int = 0
    col: int = 0
    type: object = None
    # resolved by analyzer:
    kind: str = ""                  # 'ctor', 'native', 'method', 'free'
    target_class: str = ""
    method: str = ""


@dataclass
class Member:
    """obj.field — only used transiently; becomes a Call when followed by ()."""
    target: "Expr"
    name: str
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class RunExpr:
    call: Call
    line: int = 0
    col: int = 0
    type: object = None


@dataclass
class WaitExpr:
    job: "Expr"
    line: int = 0
    col: int = 0
    type: object = None
