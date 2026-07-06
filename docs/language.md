# C-- (cmm) — Language Reference

cmm is a small, statically-checked language that compiles to portable C99 and
then to a native executable. This document covers the language itself; see the
library docs (linked from [README.md](README.md)) for the standard library and
built-in namespaces.

## A complete program

```
// hello.cmm
class Hello;
use Out;

fn main() {
    Out.line("Hello, world!");
}
```

```
cmmc run hello.cmm           # compile + run
cmmc build hello.cmm -o hi   # compile to a native binary
```

Every program has one entry class containing `fn main()`. Execution constructs
the entry class and calls its `main`.

## Files and classes

- **One class per file.** A source file begins with `class Name;` and the file
  name must match the class (`Bank` lives in `Bank.cmm`).
- Other classes are pulled in with `use OtherClass;`, which resolves a sibling
  `.cmm` file, then the bundled standard library, then any directory on
  `$CMMC_PATH`.
- A class is a namespace of fields and functions. There is no inheritance.

```
class Bank;

@balance: Int;        // class variable (state), see below

fn deposit(amount: Int) {
    @balance = (@balance + amount);
}
```

## Comments

```
// line comment
/* block
   comment */
```

## Types

| Type | Notes |
|------|-------|
| `Int` | 64-bit signed integer |
| `Float` | double-precision float |
| `Bool` | `true` / `false` |
| `String` | immutable UTF-8 text |
| `List[T]` | growable ordered list |
| `Dict[V]` | string-keyed map |
| `Data` | dynamic value — any of the above; what JSON decodes into |
| `Void` | no value (procedures) |
| `Socket` | a TCP socket handle |

The type system is **lenient**: a `List[Int]` is accepted where a `List[Data]`
is expected, and values carry a runtime tag, so `Data` flows freely. Function
signatures are checked for arity and broad compatibility, not strict generics.

## Values and variables

Assignment both declares and updates a local:

```
x = 10;            // declare
x = (x + 1);       // update
name = "Ada";
ok = true;
ratio = 3.14;
```

`empty` is the absent value (JSON `null`); reading a missing dict key or list
index yields `empty`. Test it with `==`:

```
v = doc.get("missing");
gone = (v == empty);
```

### Class variables

Declared at class scope with `@name: Type;` and referenced with `@name`:

```
@count: Int;

fn tick() {
    @count++;
}
```

## Operators

Arithmetic `+ - * / %`, comparison `== != < <= > >=`, logical `and or not`.
String concatenation uses `+`; if either side is non-string it is stringified.
`==` works on any two values.

**One operator per expression.** Compound expressions must be parenthesised so
precedence is always explicit:

```
y = (a + (b * c));     // ok
y = a + b * c;         // rejected
```

Unary: negative literals (`-1`) are fine; `not` negates a Bool. Increment and
decrement statements exist: `i++;` and `i--;` (they are statements, not
expressions, and require a variable).

## Control flow

```
if (x > 0) {
    Out.line("positive");
} else {
    Out.line("non-positive");
}

while (i < 10) {
    i++;
}

for item in items {        // iterate a List
    Out.line(item);
}
```

## Functions

```
fn add(a: Int, b: Int) -> Int {
    s = (a + b);
    return s;
}
```

- The return type is written after `->`; omit it for `Void`.
- **`return` accepts any expression** (`return a + b;`, `return foo(x);`,
  `return items.get(0);`) as well as a bare variable. The compiler captures the
  value into a temporary before leaving the frame, so returning an expression is
  exactly equivalent to assigning it to a local and returning that local.
- Call sibling functions directly (`deposit(1)`), other classes through the
  class name, and pass nested calls as arguments. There is **no method
  chaining** — bind intermediate results to variables.

## Magic constants

`__FILE__` and `__DIR__` expand at compile time to the source file's path and
its directory (the path as resolved by the compiler, typically absolute):

```
cfg = __DIR__;     // directory containing this .cmm file
self = __FILE__;   // full path of this .cmm file
```

## Concurrency

`run` starts a call on its own thread and returns a `Job`; `wait` blocks for the
result. `use <lock> { ... }` takes a lock for the block; returning from inside a
`use` block releases the lock automatically.

```
fn main() {
    j1 = run work();
    j2 = run work();
    a = wait j1;
    b = wait j2;
}

fn bump() {
    use @balance {
        @balance = (@balance + 1);
    }   // lock released here, or on early return
}
```

See [concurrency](builtin-concurrency.md) for details.

## Memory

Each function call runs in its own arena (region). Allocations made during the
call are reclaimed when it returns, except the value it returns, which is
relocated to the caller. This keeps steady-state memory low without a garbage
collector — a loop that allocates gigabytes cumulatively stays at a few MB
resident.

## Reserved words

`class use fn if else for in while return run wait empty native and or not true
false`. These cannot be used as identifiers or method names (this is why, e.g.,
the shell escape is `Sys.exec`, not `Sys.run`).

## Current limitations

- No first-class functions / lambdas, so `map` / `filter` / `reduce` are not
  provided yet.
- No inheritance or interfaces.
- One operator per expression (parenthesise compound math).
- TLS is certificate-verified against a bundled CA store (vendored mbedTLS).
