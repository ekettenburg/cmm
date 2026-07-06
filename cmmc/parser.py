"""Recursive-descent parser for the cmm language.

Produces a Module AST from a token stream. Enforces the spec's structural
rules directly in the grammar (one class per file, @-class-vars, the
"one operator per expression or else parenthesize" rule, etc.).
"""

from . import ast
from .lexer import CmmError, tokenize

BINARY_OPS = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "and", "or"}



def _src_dir(path):
    """Lexical dirname matching the C port: last '/' or '\\' separator."""
    i = max(path.rfind('/'), path.rfind('\\'))
    if i < 0:
        return "."
    if i == 0:
        return path[0]
    return path[:i]

class Parser:
    def __init__(self, tokens, filename="<input>"):
        self.toks = tokens
        self.file = filename
        self.pos = 0

    # -- token helpers --
    @property
    def cur(self):
        return self.toks[self.pos]

    def at_end(self):
        return self.cur.kind == "eof"

    def error(self, msg, tok=None):
        tok = tok or self.cur
        raise CmmError(msg, self.file, tok.line, tok.col)

    def check(self, kind, value=None):
        t = self.cur
        if t.kind != kind:
            return False
        return value is None or t.value == value

    def check_kw(self, value):
        return self.check("kw", value)

    def check_op(self, value):
        return self.check("op", value)

    def advance(self):
        t = self.cur
        if not self.at_end():
            self.pos += 1
        return t

    def match_op(self, value):
        if self.check_op(value):
            return self.advance()
        return None

    def match_kw(self, value):
        if self.check_kw(value):
            return self.advance()
        return None

    def expect_op(self, value):
        if not self.check_op(value):
            self.error(f"expected '{value}' but found '{self.cur.value}'")
        return self.advance()

    def expect_kw(self, value):
        if not self.check_kw(value):
            self.error(f"expected '{value}' but found '{self.cur.value}'")
        return self.advance()

    def expect_id(self):
        if self.cur.kind != "id":
            self.error(f"expected identifier but found '{self.cur.value}'")
        return self.advance()

    # -- top level --
    def parse_module(self):
        is_native = self.match_kw("native") is not None
        ckw = self.expect_kw("class")
        name = self.expect_id().value
        self.expect_op(";")

        uses, variables, functions = [], [], []
        while not self.at_end():
            if self.check_kw("use"):
                uses.append(self.parse_use())
            elif self.check_op("@"):
                variables.append(self.parse_classvar())
            elif self.check_kw("fn"):
                functions.append(self.parse_function(is_native))
            else:
                self.error(
                    f"expected 'use', a '@variable', or 'fn' at class scope, "
                    f"found '{self.cur.value}'"
                )
        return ast.Module(name, is_native, uses, variables, functions,
                          ckw.line, ckw.col)

    def parse_use(self):
        kw = self.expect_kw("use")
        if self.cur.kind == "string":
            url = self.advance().value
            self.expect_op(";")
            return ast.Use(None, url, kw.line, kw.col)
        # reject aliasing / wildcard forms explicitly for a clear message
        name = self.expect_id().value
        if self.check_kw("as") or self.check_op("*") or self.check_op("{"):
            self.error("only whole classes may be imported; aliases and "
                       "wildcards are not allowed")
        self.expect_op(";")
        return ast.Use(name, None, kw.line, kw.col)

    def parse_classvar(self):
        at = self.expect_op("@")
        name = self.expect_id().value
        self.expect_op(":")
        t = self.parse_type()
        self.expect_op(";")
        return ast.ClassVar(name, t, at.line, at.col)

    def parse_function(self, in_native_class):
        kw = self.expect_kw("fn")
        name = self.expect_id().value
        self.expect_op("(")
        params = []
        if not self.check_op(")"):
            params.append(self.parse_param())
            while self.match_op(","):
                if self.check_op(")"):
                    break
                params.append(self.parse_param())
        self.expect_op(")")
        ret = None
        if self.match_op("->"):
            ret = self.parse_type()
        # A native declaration (or a body-less signature) ends with ';'.
        if self.check_op(";"):
            self.advance()
            return ast.Function(name, params, ret, [], True, kw.line, kw.col)
        body = self.parse_block()
        self.match_op(";")   # tolerate the spec's trailing '};'
        return ast.Function(name, params, ret, body, in_native_class,
                            kw.line, kw.col)

    def parse_param(self):
        name = self.expect_id().value
        self.expect_op(":")
        t = self.parse_type()
        return ast.Param(name, t)

    def parse_type(self):
        tok = self.cur
        if tok.kind not in ("id",):
            self.error(f"expected a type name, found '{tok.value}'")
        name = self.advance().value
        args = []
        if self.match_op("["):
            args.append(self.parse_type())
            while self.match_op(","):
                args.append(self.parse_type())
            self.expect_op("]")
        return ast.TypeRef(name, args, tok.line, tok.col)

    # -- statements --
    def parse_block(self):
        self.expect_op("{")
        stmts = []
        while not self.check_op("}") and not self.at_end():
            stmts.append(self.parse_statement())
        self.expect_op("}")
        return stmts

    def parse_statement(self):
        if self.check_kw("if"):
            return self.parse_if()
        if self.check_kw("while"):
            return self.parse_while()
        if self.check_kw("for"):
            return self.parse_for()
        if self.check_kw("return"):
            return self.parse_return()
        if self.check_kw("use"):
            return self.parse_uselock()

        # Otherwise: assignment / empty-decl / inc-dec / expression statement
        start = self.cur
        lhs = self.parse_expression()

        if self.match_op(":"):
            if not isinstance(lhs, ast.Name):
                self.error("empty declaration must name a simple variable", start)
            t = self.parse_type()
            self.expect_op(";")
            return ast.EmptyDecl(lhs.name, t, start.line, start.col)

        if self.match_op("="):
            value = self.parse_expression()
            self.expect_op(";")
            if not isinstance(lhs, (ast.Name, ast.ClassVarRef, ast.Index)):
                self.error("invalid assignment target", start)
            return ast.Assign(lhs, value, False, start.line, start.col)

        if self.check_op("++") or self.check_op("--"):
            op = self.advance().value
            self.expect_op(";")
            if not isinstance(lhs, (ast.Name, ast.ClassVarRef)):
                self.error("'++'/'--' require a variable", start)
            return ast.IncDec(lhs, op, start.line, start.col)

        self.expect_op(";")
        return ast.ExprStmt(lhs, start.line, start.col)

    def parse_if(self):
        kw = self.expect_kw("if")
        cond = self.parse_expression()
        then = self.parse_block()
        otherwise = None
        # tolerate '}' ';' before else
        self.match_op(";")
        if self.match_kw("else"):
            if self.check_kw("if"):
                otherwise = [self.parse_if()]
            else:
                otherwise = self.parse_block()
                self.match_op(";")
        return ast.If(cond, then, otherwise, kw.line, kw.col)

    def parse_while(self):
        kw = self.expect_kw("while")
        cond = self.parse_expression()
        body = self.parse_block()
        self.match_op(";")
        return ast.While(cond, body, kw.line, kw.col)

    def parse_for(self):
        kw = self.expect_kw("for")
        var = self.expect_id().value
        self.expect_kw("in")
        iterable = self.parse_expression()
        body = self.parse_block()
        self.match_op(";")
        return ast.For(var, iterable, body, kw.line, kw.col)

    def parse_return(self):
        kw = self.expect_kw("return")
        value = None
        if not self.check_op(";"):
            value = self.parse_expression()
        self.expect_op(";")
        return ast.Return(value, kw.line, kw.col)

    def parse_uselock(self):
        kw = self.expect_kw("use")
        names = []
        self.expect_op("@")
        names.append(self.expect_id().value)
        while self.match_op(","):
            self.expect_op("@")
            names.append(self.expect_id().value)
        body = self.parse_block()
        self.match_op(";")
        return ast.UseLock(names, body, kw.line, kw.col)

    # -- expressions --
    def parse_expression(self):
        left = self.parse_unary()
        if self.cur.value in BINARY_OPS and self.cur.kind in ("op", "kw"):
            op = self.advance().value
            right = self.parse_unary()
            # Enforce the spec rule: at most one operator per group.
            if self.cur.value in BINARY_OPS and self.cur.kind in ("op", "kw"):
                self.error(
                    "compound expression must be grouped with parentheses "
                    "(e.g. 'a + (b * c)')"
                )
            return ast.Binary(op, left, right, left.line, left.col)
        return left

    def parse_unary(self):
        if self.check_kw("not"):
            t = self.advance()
            return ast.Unary("not", self.parse_unary(), t.line, t.col)
        if self.check_op("-"):
            t = self.advance()
            return ast.Unary("-", self.parse_unary(), t.line, t.col)
        if self.check_kw("empty"):
            t = self.advance()
            self.expect_op("(")
            e = self.parse_expression()
            self.expect_op(")")
            return ast.EmptyCheck(e, t.line, t.col)
        return self.parse_postfix()

    def parse_postfix(self):
        e = self.parse_primary()
        while True:
            if self.match_op("."):
                name = self.expect_id()
                e = ast.Member(e, name.value, e.line, e.col)
            elif self.check_op("["):
                self.advance()
                idx = self.parse_expression()
                self.expect_op("]")
                e = ast.Index(e, idx, e.line, e.col)
            elif self.check_op("("):
                args = self.parse_args()
                e = ast.Call(e, args, e.line, e.col)
            else:
                break
        return e

    def parse_args(self):
        self.expect_op("(")
        args = []
        if not self.check_op(")"):
            args.append(self.parse_expression())
            while self.match_op(","):
                if self.check_op(")"):
                    break
                args.append(self.parse_expression())
        self.expect_op(")")
        return args

    def parse_primary(self):
        t = self.cur
        if t.kind == "int":
            self.advance()
            return ast.IntLit(int(t.value), t.line, t.col)
        if t.kind == "float":
            self.advance()
            return ast.FloatLit(float(t.value), t.line, t.col)
        if t.kind == "string":
            self.advance()
            return ast.StringLit(t.value, t.line, t.col)
        if self.check_kw("true"):
            self.advance()
            return ast.BoolLit(True, t.line, t.col)
        if self.check_kw("false"):
            self.advance()
            return ast.BoolLit(False, t.line, t.col)
        if self.check_kw("run"):
            self.advance()
            call = self.parse_postfix()
            if not isinstance(call, ast.Call):
                self.error("'run' must be followed by a call expression", t)
            return ast.RunExpr(call, t.line, t.col)
        if self.check_kw("wait"):
            self.advance()
            job = self.parse_postfix()
            return ast.WaitExpr(job, t.line, t.col)
        if self.match_op("("):
            e = self.parse_expression()
            self.expect_op(")")
            return e
        if self.check_op("["):
            return self.parse_list_literal()
        if self.check_op("{"):
            return self.parse_dict_literal()
        if self.check_op("@"):
            self.advance()
            name = self.expect_id()
            return ast.ClassVarRef(name.value, t.line, t.col)
        if t.kind == "id":
            self.advance()
            if t.value == "__FILE__":
                return ast.StringLit(self.file, t.line, t.col)
            if t.value == "__DIR__":
                return ast.StringLit(_src_dir(self.file), t.line, t.col)
            return ast.Name(t.value, t.line, t.col)
        self.error(f"unexpected '{t.value}' in expression")

    def parse_list_literal(self):
        t = self.expect_op("[")
        items = []
        if not self.check_op("]"):
            items.append(self.parse_expression())
            while self.match_op(","):
                if self.check_op("]"):
                    break
                items.append(self.parse_expression())
        self.expect_op("]")
        return ast.ListLit(items, t.line, t.col)

    def parse_dict_literal(self):
        t = self.expect_op("{")
        keys, values = [], []
        if not self.check_op("}"):
            k = self.parse_expression()
            self.expect_op(":")
            v = self.parse_expression()
            keys.append(k)
            values.append(v)
            while self.match_op(","):
                if self.check_op("}"):
                    break
                k = self.parse_expression()
                self.expect_op(":")
                v = self.parse_expression()
                keys.append(k)
                values.append(v)
        self.expect_op("}")
        return ast.DictLit(keys, values, t.line, t.col)


def parse(source, filename="<input>"):
    tokens = tokenize(source, filename)
    return Parser(tokens, filename).parse_module()
