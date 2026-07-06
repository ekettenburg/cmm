"""Semantic analysis for the cmm language.

Operates over a whole program (a set of modules keyed by class name). It:
  * builds class signatures (variable types/indices, function signatures),
  * infers a static Type for every expression node (stored on node.type),
  * resolves every Call to a concrete kind + target,
  * enforces the spec's semantic rules.
"""

from . import ast
from . import types as T
from .lexer import CmmError


class FuncSig:
    def __init__(self, fn, owner):
        self.name = fn.name
        self.params = [(p.name, T.type_from_ref(p.type)) for p in fn.params]
        self.ret = T.type_from_ref(fn.ret) if fn.ret else T.VOID
        self.owner = owner
        self.node = fn


class ClassSig:
    def __init__(self, module):
        self.name = module.classname
        self.module = module
        self.vars = {}        # name -> (Type, index)
        for i, v in enumerate(module.variables):
            self.vars[v.name] = (T.type_from_ref(v.type), i)
        self.funcs = {f.name: FuncSig(f, self.name) for f in module.functions}


class Scope:
    def __init__(self):
        self.vars = {}        # name -> Type

    def has(self, n):
        return n in self.vars

    def get(self, n):
        return self.vars.get(n)

    def declare(self, n, t):
        self.vars[n] = t


class Analyzer:
    def __init__(self, program, entry):
        # program: dict classname -> Module
        self.program = program
        self.entry = entry
        self.classes = {name: ClassSig(m) for name, m in program.items()}
        self.file = "<program>"
        self.collect = False     # lint mode: gather all errors instead of failing fast
        self.errors = []

    def err(self, msg, node):
        e = CmmError(msg, self.file, getattr(node, "line", 0),
                     getattr(node, "col", 0))
        if self.collect:
            self.errors.append(e)
        raise e

    def analyze(self):
        for name, csig in self.classes.items():
            self.file = csig.module.classname + ".cmm"
            # record var index on the module for codegen
            csig.module.var_index = {v: i for v, (t, i) in csig.vars.items()}
            for fn in csig.module.functions:
                if not fn.is_native and fn.body is not None and not csig.module.is_native:
                    if self.collect:
                        try:
                            self.analyze_function(csig, fn)
                        except CmmError:
                            pass
                    else:
                        self.analyze_function(csig, fn)
        return self.program

    # -- function/body --
    def analyze_function(self, csig, fn):
        self.cur_class = csig
        self.cur_fn = fn
        scope = Scope()
        for p in fn.params:
            scope.declare(p.name, T.type_from_ref(p.type))
        self.block(fn.body, scope)

    def block(self, stmts, scope):
        for s in stmts:
            if self.collect:
                try:
                    self.stmt(s, scope)
                except CmmError:
                    pass   # recorded in err(); keep going to find more
            else:
                self.stmt(s, scope)

    # -- statements --
    def stmt(self, s, scope):
        m = getattr(self, "stmt_" + type(s).__name__, None)
        if m is None:
            self.err(f"cannot analyze statement {type(s).__name__}", s)
        m(s, scope)

    def stmt_Assign(self, s, scope):
        if self.collect:
            try:
                vt = self.expr(s.value, scope)
            except CmmError:
                # lint recovery: the RHS is broken, but still declare a new
                # simple target as permissive so later uses don't cascade.
                t = s.target
                if isinstance(t, ast.Name) and scope.get(t.name) is None:
                    scope.declare(t.name, T.DATA)
                raise
        else:
            vt = self.expr(s.value, scope)
        tgt = s.target
        if isinstance(tgt, ast.Name):
            existing = scope.get(tgt.name)
            if existing is None:
                if vt == T.VOID:
                    self.err("cannot assign a value-less expression", s)
                scope.declare(tgt.name, vt)
                s.declares = True
                tgt.type = vt
            else:
                if existing != vt and not self._assignable(existing, vt):
                    self.err(
                        f"variable '{tgt.name}' has type {existing}; cannot "
                        f"reassign it to {vt} (a local's type is fixed at first "
                        f"assignment)", s)
                tgt.type = existing
        elif isinstance(tgt, ast.ClassVarRef):
            t = self._classvar_type(tgt.name, s)
            tgt.type = t
            if not self._assignable(t, vt):
                self.err(f"cannot assign {vt} to @{tgt.name} of type {t}", s)
        elif isinstance(tgt, ast.Index):
            self.expr(tgt, scope)
        else:
            self.err("invalid assignment target", s)

    def stmt_EmptyDecl(self, s, scope):
        if scope.has(s.name):
            self.err(f"'{s.name}' is already declared", s)
        scope.declare(s.name, T.type_from_ref(s.type))

    def stmt_IncDec(self, s, scope):
        t = self.expr(s.target, scope)
        if not t.is_numeric():
            self.err(f"'{s.op}' requires a numeric variable, got {t}", s)

    def stmt_ExprStmt(self, s, scope):
        self.expr(s.expr, scope)

    def stmt_Return(self, s, scope):
        if s.value is None:
            return
        # A return may carry any expression; codegen captures it into a
        # temporary (__ret) before leaving the frame, so no restriction here.
        rt = self.expr(s.value, scope)
        want = self.cur_fn.ret and T.type_from_ref(self.cur_fn.ret) or T.VOID
        # (we don't hard-fail on mismatch; tagged values are permissive)

    def stmt_If(self, s, scope):
        ct = self.expr(s.cond, scope)
        self.block(s.then, Scope_child(scope))
        if s.otherwise is not None:
            self.block(s.otherwise, Scope_child(scope))

    def stmt_While(self, s, scope):
        self.expr(s.cond, scope)
        self.block(s.body, Scope_child(scope))

    def stmt_For(self, s, scope):
        it = self.expr(s.iterable, scope)
        if it.name == "List":
            elem = it.args[0] if it.args else T.DATA
        elif it.name == "String":
            elem = T.STRING
        elif it.name == "Dict":
            elem = T.STRING            # iterate keys
        elif it.name == "Data":
            elem = T.DATA
        else:
            self.err(f"cannot iterate over a value of type {it}", s)
        child = Scope_child(scope)
        child.declare(s.var, elem)
        s.elem_type = elem
        self.block(s.body, child)

    def stmt_UseLock(self, s, scope):
        for n in s.vars:
            if n not in self.cur_class.vars:
                self.err(f"'use' lock target @{n} is not a class variable", s)
        self.block(s.body, Scope_child(scope))

    # -- expressions --
    def expr(self, e, scope):
        m = getattr(self, "expr_" + type(e).__name__, None)
        if m is None:
            self.err(f"cannot analyze expression {type(e).__name__}", e)
        t = m(e, scope)
        e.type = t
        return t

    def expr_IntLit(self, e, scope):    return T.INT
    def expr_FloatLit(self, e, scope):  return T.FLOAT
    def expr_StringLit(self, e, scope): return T.STRING
    def expr_BoolLit(self, e, scope):   return T.BOOL

    def expr_ListLit(self, e, scope):
        ts = [self.expr(it, scope) for it in e.items]
        elem = ts[0] if ts and all(x == ts[0] for x in ts) else T.DATA
        return T.list_of(elem)

    def expr_DictLit(self, e, scope):
        for k in e.keys:
            self.expr(k, scope)
        vts = [self.expr(v, scope) for v in e.values]
        val = vts[0] if vts and all(x == vts[0] for x in vts) else T.DATA
        return T.dict_of(T.STRING, val)

    def expr_Name(self, e, scope):
        t = scope.get(e.name)
        if t is not None:
            return t
        # bare class name used as a namespace / constructor target
        if e.name in T.NATIVE_NAMESPACES or e.name in T.NATIVE_CLASSES:
            return T.namespace(e.name)
        if e.name in self.classes:
            return T.namespace(e.name)
        self.err(f"undefined name '{e.name}'", e)

    def expr_ClassVarRef(self, e, scope):
        return self._classvar_type(e.name, e)

    def expr_Unary(self, e, scope):
        t = self.expr(e.operand, scope)
        if e.op == "not":
            return T.BOOL
        return t  # '-'

    def expr_EmptyCheck(self, e, scope):
        self.expr(e.operand, scope)
        return T.BOOL

    def expr_Binary(self, e, scope):
        lt = self.expr(e.left, scope)
        rt = self.expr(e.right, scope)
        if e.op in ("==", "!=", "<", "<=", ">", ">=", "and", "or"):
            return T.BOOL
        if e.op == "+":
            if lt == T.STRING or rt == T.STRING:
                return T.STRING
        if lt == T.FLOAT or rt == T.FLOAT:
            return T.FLOAT
        if lt == T.INT and rt == T.INT:
            return T.INT
        # Data / mixed arithmetic falls back to Float-ish numeric; keep Data
        if lt == T.DATA or rt == T.DATA:
            return T.DATA
        return lt

    def expr_Index(self, e, scope):
        tt = self.expr(e.target, scope)
        self.expr(e.index, scope)
        if tt.name == "List":
            return tt.args[0] if tt.args else T.DATA
        if tt.name == "Dict":
            return tt.args[1] if len(tt.args) > 1 else T.DATA
        if tt.name == "String":
            return T.STRING
        if tt.name == "Data":
            return T.DATA
        self.err(f"cannot index a value of type {tt}", e)

    def expr_Member(self, e, scope):
        # A bare member access without a call is only meaningful for Data.
        tt = self.expr(e.target, scope)
        if tt.name in ("Data",):
            return T.DATA
        self.err(f"'{e.name}' must be called as a method", e)

    def expr_RunExpr(self, e, scope):
        ct = self.expr(e.call, scope)
        return T.job_of(ct if ct != T.VOID else T.DATA)

    def expr_WaitExpr(self, e, scope):
        jt = self.expr(e.job, scope)
        if jt.name == "Job":
            return jt.args[0] if jt.args else T.DATA
        self.err(f"'wait' expects a Job, got {jt}", e)

    def expr_Call(self, e, scope):
        callee = e.callee
        # Constructor / sibling-method / free call:  Name(...)
        if isinstance(callee, ast.Name) and not scope.has(callee.name):
            name = callee.name
            if name in self.classes and name not in T.NATIVE_NAMESPACES:
                csig = self.classes[name]
                e.kind = "ctor"
                e.target_class = name
                for a in e.args:
                    self.expr(a, scope)
                return T.Type(name)
            # sibling method on current instance
            if name in self.cur_class.funcs:
                sig = self.cur_class.funcs[name]
                e.kind = "self_method"
                e.target_class = self.cur_class.name
                e.method = name
                for a in e.args:
                    self.expr(a, scope)
                return sig.ret
            self.err(f"unknown function or class '{name}'", e)

        # Member call:  target.method(...)
        if isinstance(callee, ast.Member):
            target = callee.target
            method = callee.name
            # Namespace call:  Console.print(...)
            if isinstance(target, ast.Name) and not scope.has(target.name) \
                    and target.name in T.NATIVE_NAMESPACES:
                key = (target.name, method)
                if key not in T.NAMESPACE_METHODS:
                    self.err(f"{target.name} has no method '{method}'", e)
                cfn, ret, arity = T.NAMESPACE_METHODS[key]
                if arity is not None and len(e.args) != arity:
                    self.err(f"{target.name}.{method} expects {arity} "
                             f"argument(s), got {len(e.args)}", e)
                for a in e.args:
                    self.expr(a, scope)
                e.kind = "native"
                e.target_class = target.name
                e.method = method
                return ret
            # User-class static call:  ClassName.method(...)  ->  call the
            # method with a fresh empty instance as the receiver. Utility
            # (stdlib) classes use this; such methods should not rely on @state.
            if isinstance(target, ast.Name) and not scope.has(target.name) \
                    and target.name in self.classes \
                    and target.name not in T.NATIVE_NAMESPACES:
                base = target.name
                csig = self.classes[base]
                if method not in csig.funcs:
                    self.err(f"class {base} has no static method '{method}'", e)
                sig = csig.funcs[method]
                for a in e.args:
                    self.expr(a, scope)
                if len(e.args) != len(sig.params):
                    self.err(f"{base}.{method} expects {len(sig.params)} "
                             f"argument(s), got {len(e.args)}", e)
                e.kind = "static"
                e.target_class = base
                e.method = method
                return sig.ret
            recv = self.expr(target, scope)
            for a in e.args:
                self.expr(a, scope)
            # Value-type method?
            base = recv.name
            if base in T.VALUE_METHODS and method in T.VALUE_METHODS[base]:
                cfn, ret, arity = T.VALUE_METHODS[base][method]
                if callable(ret):
                    ret = ret(recv)
                if arity is not None and len(e.args) != arity:
                    self.err(f"{base}.{method} expects {arity} argument(s), "
                             f"got {len(e.args)}", e)
                e.kind = "value_method"
                e.target_class = base
                e.method = method
                return ret
            # User class instance method
            if base in self.classes:
                csig = self.classes[base]
                if method not in csig.funcs:
                    self.err(f"class {base} has no method '{method}'", e)
                e.kind = "method"
                e.target_class = base
                e.method = method
                return csig.funcs[method].ret
            self.err(f"type {recv} has no method '{method}'", e)

        self.err("invalid call", e)

    # -- helpers --
    def _classvar_type(self, name, node):
        if name not in self.cur_class.vars:
            self.err(f"@{name} is not a declared class variable", node)
        return self.cur_class.vars[name][0]

    def _assignable(self, target, value):
        if target == value:
            return True
        # Data accepts anything; anything accepts Data (dynamic).
        if target == T.DATA or value == T.DATA:
            return True
        # numeric widening Int -> Float
        if target == T.FLOAT and value == T.INT:
            return True
        # empty containers unify with declared containers
        if target.name == value.name in ("List", "Dict", "Job"):
            return True
        return False


def Scope_child(parent):
    """cmm has function-level scoping for locals; nested blocks see and add
    to the same set of locals (matching the spec's simple model)."""
    return parent
