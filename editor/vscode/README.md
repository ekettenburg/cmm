# C-- (cmm) for VS Code

Syntax highlighting and live diagnostics for the [C-- (cmm)](../../README.md)
language.

## Features

- **Syntax highlighting** for `.cmm` files — keywords, types, the built-in
  namespaces (`Console`, `Crypto`, `Mysql`, `Preg`, …), strings, comments,
  numbers, `@class` variables, and magic constants.
- **Live diagnostics (linting)** — errors from the compiler appear in the
  Problems panel and as squiggles as you type. Powered by `cmmc check`, so the
  messages match exactly what a build would report.

## Requirements

The `cmmc` compiler must be installed. By default the extension runs `cmmc`
from your `PATH`; point it elsewhere with the **`cmm.compilerPath`** setting
(e.g. `/path/to/cmm/bootstrap/cmmc`).

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `cmm.compilerPath` | `cmmc` | Path to the `cmmc` binary used for diagnostics. |
| `cmm.lint.enable` | `true` | Turn diagnostics on/off. |
| `cmm.lint.run` | `onType` | `onType` (as you edit) or `onSave`. |

## Install (from source)

```sh
# copy or symlink this folder into your VS Code extensions dir:
cp -r editor/vscode ~/.vscode/extensions/cmm-language-0.7.0
# then reload VS Code
```

Or package it with `vsce package` (needs `npm i -g @vscode/vsce`) to produce a
`.vsix`, then "Install from VSIX…" in the Extensions view.

## Debugging cmm programs

Build with `-g/--debug` for `-O0`, DWARF symbols, and `#line` directives that
map the generated C back to your `.cmm` source, so `gdb`/`lldb` step through the
original file:

```sh
cmmc build myprog.cmm -o myprog --debug
gdb ./myprog
```
