# Math and Date (built-in)

## Math

| Method | Signature | Description |
|--------|-----------|-------------|
| `abs` | `(x: Float) -> Float` | absolute value |
| `floor` | `(x: Float) -> Int` | round down |
| `ceil` | `(x: Float) -> Int` | round up |
| `min` | `(a: Float, b: Float) -> Float` | smaller of two |
| `max` | `(a: Float, b: Float) -> Float` | larger of two |
| `pow` | `(base: Float, exp: Float) -> Float` | exponentiation |
| `sqrt` | `(x: Float) -> Float` | square root |
| `pi` | `() -> Float` | the constant π |
| `random` | `() -> Float` | uniform random in `[0, 1)` |

```
r = Math.sqrt(2.0);
hi = Math.max(3.0, 9.0);
area = (Math.pi() * (r * r));
```

## Date

| Method | Signature | Description |
|--------|-----------|-------------|
| `now` | `() -> Int` | current Unix time (seconds) |
| `format` | `(epoch: Int, fmt: String) -> String` | format a timestamp (`strftime` patterns) |

```
t = Date.now();
stamp = Date.format(t, "%Y-%m-%d %H:%M:%S");
```

## PHP-style date formatting

`Date.date(format, epoch)` (local time) and `Date.gmdate(format, epoch)` (UTC)
format a Unix timestamp using PHP `date()` format characters. `Date.now()`
returns the current epoch seconds; `Date.amzDate()` returns the current UTC time
as `YYYYMMDDTHHMMSSZ` (for AWS SigV4).

```cmm
Date.gmdate("Y-m-d H:i:s", 1700000000);   // 2023-11-14 22:13:20
Date.gmdate("l, F j, Y", 1700000000);     // Tuesday, November 14, 2023
Date.gmdate("D jS M 'y g:i A", 1700000000); // Tue 14th Nov '23 10:13 PM
Date.gmdate("c", 1700000000);             // 2023-11-14T22:13:20+00:00
Date.gmdate("r", 1700000000);             // Tue, 14 Nov 2023 22:13:20 +0000
```

Supported format characters:

| | | | |
|--|--|--|--|
| `d` day 01-31 | `j` day 1-31 | `D` Mon | `l` Monday |
| `N` ISO 1-7 | `w` 0-6 | `S` st/nd/rd/th | `z` day-of-year 0-365 |
| `W` ISO week | `o` ISO year | `F` January | `M` Jan |
| `m` 01-12 | `n` 1-12 | `t` days in month | `L` leap 1/0 |
| `Y` 2023 | `y` 23 | `a` am/pm | `A` AM/PM |
| `g` 1-12 | `G` 0-23 | `h` 01-12 | `H` 00-23 |
| `i` minutes | `s` seconds | `U` epoch | `T`/`e` zone |
| `O` +0000 | `P` +00:00 | `Z` offset secs | `c` ISO 8601 |
| `r` RFC 2822 | `u` micro (000000) | `v` milli (000) | `\` escape next |

Unrecognized characters pass through unchanged; prefix a character with `\` to
emit it literally.
