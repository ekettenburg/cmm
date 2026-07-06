"""Lexer for the cmm language.

Turns source text into a flat list of Token objects. No significant
whitespace; statements end with ';' and blocks use '{' '}'.
"""

from dataclasses import dataclass


class CmmError(Exception):
    """A compile-time error with file/line/col location."""

    def __init__(self, message, file="<input>", line=0, col=0):
        self.message = message
        self.file = file
        self.line = line
        self.col = col
        super().__init__(self.format())

    def format(self):
        return f"{self.file}:{self.line}:{self.col}: error: {self.message}"


KEYWORDS = {
    "class", "use", "fn", "if", "else", "for", "in", "while",
    "return", "run", "wait", "empty", "native",
    "and", "or", "not", "true", "false",
}

# Multi-char operators, longest first so the scanner is greedy.
OPERATORS = [
    "->", "==", "!=", "<=", ">=", "++", "--",
    "+", "-", "*", "/", "%", "=", "<", ">",
    "{", "}", "(", ")", "[", "]",
    ";", ":", ",", ".", "@",
]


@dataclass
class Token:
    kind: str        # 'kw', 'id', 'int', 'float', 'string', 'op', 'eof'
    value: str
    line: int
    col: int

    def __repr__(self):
        return f"Token({self.kind}, {self.value!r}, {self.line}:{self.col})"


class Lexer:
    def __init__(self, source, filename="<input>"):
        self.src = source
        self.file = filename
        self.i = 0
        self.line = 1
        self.col = 1
        self.tokens = []

    def error(self, msg):
        raise CmmError(msg, self.file, self.line, self.col)

    def peek(self, offset=0):
        j = self.i + offset
        return self.src[j] if j < len(self.src) else ""

    def advance(self):
        c = self.src[self.i]
        self.i += 1
        if c == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return c

    def add(self, kind, value, line, col):
        self.tokens.append(Token(kind, value, line, col))

    def tokenize(self):
        while self.i < len(self.src):
            c = self.peek()
            # Whitespace
            if c in " \t\r\n":
                self.advance()
                continue
            # Line comment
            if c == "/" and self.peek(1) == "/":
                while self.i < len(self.src) and self.peek() != "\n":
                    self.advance()
                continue
            # Block comment
            if c == "/" and self.peek(1) == "*":
                self.advance()
                self.advance()
                while self.i < len(self.src) and not (self.peek() == "*" and self.peek(1) == "/"):
                    self.advance()
                if self.i >= len(self.src):
                    self.error("unterminated block comment")
                self.advance()
                self.advance()
                continue
            # String literal
            if c == '"':
                self.read_string()
                continue
            # Number
            if c.isdigit():
                self.read_number()
                continue
            # Identifier / keyword
            if c.isalpha() or c == "_":
                self.read_word()
                continue
            # Operator / punctuation
            if not self.read_operator():
                self.error(f"unexpected character {c!r}")
        self.add("eof", "", self.line, self.col)
        return self.tokens

    def read_string(self):
        line, col = self.line, self.col
        self.advance()  # opening quote
        out = []
        while True:
            if self.i >= len(self.src):
                raise CmmError("unterminated string literal", self.file, line, col)
            c = self.advance()
            if c == '"':
                break
            if c == "\\":
                e = self.advance()
                out.append({
                    "n": "\n", "t": "\t", "r": "\r", "\\": "\\",
                    '"': '"', "0": "\0",
                }.get(e, e))
            else:
                out.append(c)
        self.add("string", "".join(out), line, col)

    def read_number(self):
        line, col = self.line, self.col
        start = self.i
        is_float = False
        while self.i < len(self.src) and self.peek().isdigit():
            self.advance()
        if self.peek() == "." and self.peek(1).isdigit():
            is_float = True
            self.advance()
            while self.i < len(self.src) and self.peek().isdigit():
                self.advance()
        text = self.src[start:self.i]
        self.add("float" if is_float else "int", text, line, col)

    def read_word(self):
        line, col = self.line, self.col
        start = self.i
        while self.i < len(self.src) and (self.peek().isalnum() or self.peek() == "_"):
            self.advance()
        text = self.src[start:self.i]
        self.add("kw" if text in KEYWORDS else "id", text, line, col)

    def read_operator(self):
        line, col = self.line, self.col
        for op in OPERATORS:
            if self.src.startswith(op, self.i):
                for _ in op:
                    self.advance()
                self.add("op", op, line, col)
                return True
        return False


def tokenize(source, filename="<input>"):
    return Lexer(source, filename).tokenize()
