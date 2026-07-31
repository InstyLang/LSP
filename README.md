# Insty Language Server

`insty-lsp` is the language server for the [Insty](https://github.com/InstyLang)
language. It reuses the Insty compiler's lexer, parser, and semantic analyzer to
provide real diagnostics, completion, hover, go-to-definition, find-references,
rename, and semantic tokens.

## Capabilities

- Diagnostics from the compiler lexer, parser, and semantic analyzer (plus
  unresolved-import and unused-local hints)
- Completion for keywords, types, builtins, visible symbols, imported modules,
  selective imports, and wildcard imports
- Hover, go to definition, find references, rename
- Semantic tokens: parser-accurate highlighting that distinguishes functions,
  methods, parameters, variables, properties, types, classes, enums, enum
  variants (sum-type payloads), type parameters, keywords, numbers, strings, and
  `@builtin` macros — including inside `for`-loops and `match` arms
- Both stdio and TCP (`--socket <port>`) transports

## Dependency on the compiler

The server links against the Insty compiler frontend (lexer + parser) directly,
so a checkout of the [Compiler](https://github.com/InstyLang/Compiler) repo must
be available. By default CMake expects it at `../Compiler`:

```
parent/
  Compiler/      # github.com/InstyLang/Compiler
  LSP/           # this repo
```

Point it elsewhere with `-DINSTY_COMPILER_DIR=<path-to-compiler>`.

## Build

```bash
cmake -S . -B build
cmake --build build --target insty-lsp
```

The binary is `build/insty-lsp`.

## Editor integration

The VS Code client lives in the
[extension](https://github.com/InstyLang/extension) repo. It looks for
`insty-lsp` on `PATH` (configurable via `insty.lspPath`) and supports both
stdio and TCP transports.
