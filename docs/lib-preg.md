# Preg — PHP-style regular expressions

Always available, no import required. A backtracking regex engine (PCRE-style)
with a PHP `preg_*` flavour. Patterns are **delimited** like `/regex/flags`.

> In cmm string literals an unknown escape drops its backslash (`"\d"` → `"d"`),
> so write regex backslashes **doubled**: `"/\\d+/"`, `"/\\bword\\b/"`.

## Functions

| Call | Returns | PHP analogue |
|------|---------|--------------|
| `Preg.match(pattern, subject)` | `List[String]` | `preg_match` — `[full, $1, $2, …]`, empty list if no match |
| `Preg.test(pattern, subject)` | `Bool` | `preg_match` as a boolean |
| `Preg.matchAll(pattern, subject)` | `List[String]` | `preg_match_all` — every full match |
| `Preg.replace(pattern, replacement, subject)` | `String` | `preg_replace` — `$1`/`${1}`/`\1` backrefs |
| `Preg.split(pattern, subject)` | `List[String]` | `preg_split` |
| `Preg.quote(str)` | `String` | `preg_quote` |

## Flags

`i` case-insensitive · `m` multiline (`^`/`$` match at line breaks) · `s` dotall
(`.` matches newline).

## Supported syntax

Literals · `.` · character classes `[...]`, ranges `[a-z]`, negation `[^...]` ·
shorthands `\d \D \w \W \s \S` · anchors `^ $ \b \B` · quantifiers
`* + ? {n} {n,} {n,m}` and lazy `*? +? ?? {n,m}?` · groups `( )` and
non-capturing `(?: )` · alternation `|` · backreferences `\1`–`\9`.

```cmm
// capture groups
parts = Preg.match("/(\\d{4})-(\\d{2})-(\\d{2})/", "2026-06-30");
// -> ["2026-06-30", "2026", "06", "30"]

// replace with backreferences
Preg.replace("/(\\w+)\\s+(\\w+)/", "$2 $1", "hello world");   // "world hello"

// validate + split
Preg.test("/^[a-z0-9._%+-]+@[a-z0-9.-]+\\.[a-z]{2,}$/i", "a@b.com");  // true
Preg.split("/\\s*,\\s*/", "a, b ,c");                          // ["a","b","c"]
```

Note: this is a backtracking engine with a step budget, so pathological patterns
(catastrophic backtracking) fail closed as "no match" rather than hang.
