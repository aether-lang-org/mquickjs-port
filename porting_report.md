# MicroQuickJS → Aether porting report

Status of the port of MicroQuickJS to [Aether](../aether/LLM.md), built with
[aeb](../aeb/) and tested partly with [aeocha](../aeocha/).

**`mquickjs.c` is deleted — the entire engine is Aether.** The 18,366-line C
engine file is gone; the engine now links from **227 Aether modules** under
`ae/` plus the third-party `dtoa.c` and `libm.c`. Every gate stays green: the
`aeb .tests.ae` conformance suite at 7/7, the aeocha unit suites, the GC-stress
program (`14999850000`), the bytecode round-trip, the `-m32` cross-compile, and
syntax-error reporting — all verified, with the regex / value-printer /
backtrace / `-m32` output byte-identical to the prior build.

## What made deleting the engine file possible

Two Aether language features dissolved the last barriers I had previously
treated as irreducible C:

- **Aether `try` / `catch` / `panic`** replaced the `setjmp`/`longjmp` parse
  harness. `JS_Parse2` now allocates the parse state on the heap, wraps the run
  path in `try { … } catch { … }`, and the parse-error leaves `panic()` instead
  of `longjmp`. (The known frame-leak gotcha — a `return` inside a try/catch
  body — is avoided by stashing the outcome in a local and returning once after
  the block.)
- **Aether `const` arrays** replaced the C static-const data tables
  (`opcode_info[]`, `reopcode_info[]`, `char_range_s/w[]`). Consumers index the
  Aether arrays directly.

With those, plus the earlier `va_start`/`va_arg` (printf family) and typed
function-pointer dispatch (VM trampolines), nothing engine-side needed C: the
remaining static-inline helpers and macros became dead and went with the file.

## Remaining C by file

| File | Lines | What it is |
|---|---:|---|
| **libm.c** | 2,260 | third-party math library |
| **dtoa.c** | 1,618 | third-party float↔string conversion (Bellard's `mp_*` bignum) |
| **mquickjs_build.c** | 869 | host stdlib-baking codegen tool |
| **readline.c** | 742 | terminal line editor (third-party) |
| **mqjs.c** | 723 | CLI host shell |
| **mqjs_stdlib.c** | 407 | the stdlib *definition* (data fed to the build tool) |
| **example.c** | 308 | embedding demo |
| **readline_tty.c** | 246 | terminal raw-mode glue |
| **example_stdlib.c** | 36 | demo's custom-class stdlib definition |
| **total** | **7,209** | (no `mquickjs.c`) |

The engine's C source set (`gen/mqjssources`) is now just `dtoa.c`, `libm.c`,
and the layout-guard.

## Engine: fully Aether

Everything the engine does is now Aether, across 227 modules:

- value representation, tag encoding, the type/struct overlays
- the allocator, GC (mark / compact / threading), the property core
  (get/set/define/delete, rehash, compact)
- all builtins (Array, String, Object, Number, Boolean, JSON, Math, RegExp,
  TypedArray, Date, Error, Function)
- the parser (recursive-descent expression/statement ladder, the regex
  compiler, the JSON parser) — with parse errors now via `try`/`catch`/`panic`
- the regex **runtime** engine `lre_exec`
- the bytecode VM (`JS_Call`), the printf family, `JS_ThrowError`, the value
  pretty-printer, `build_backtrace`, the memory dumper
- the dtoa-facing wrappers, the same-width bytecode save/relocate/load, and the
  64→32 cross-compilation tools
- the opcode / regexp-opcode / character-range data tables (Aether `const`
  arrays)

## CLI (`mqjs.c`): builtins ported, host shell stays C

The portable JS builtins moved to `ae/cli_host.ae`: `print`, `gc`, `load`,
`Date` (constructor + `Date.now`), `performance.now`, plus the REPL
syntax-highlighter helpers. What stays C in `mqjs.c` owns host state or drives
the OS: the `setTimeout`/`clearTimeout` timer machinery (a static timer array +
`nanosleep`), the file/eval/compile glue (which uses the static `term_colors`
table, the log-redirect flag, and `perror`/`fprintf`/`exit`), the terminal
colorizer, the readline completion hook, the REPL loop, the interrupt handler,
and `main` (argv/getopt + signal setup).

## Build tool (`mquickjs_build.c` + `mqjs_stdlib.c`): left as host machinery

The host stdlib-baking codegen tool and its stdlib spec were left in C by
choice — they are host build machinery (like `dtoa.c`/`libm.c`), not part of
the engine or CLI. Their pure logic (atom/cfunc list management, predicates)
was already moved to Aether libraries linked into the tool; the remaining C is
the printf C-text emission and the declarative spec.

## Verification

- `aeb .tests.ae` conformance gate: **7/7** (4 JS suites + bytecode round-trip + embed)
- aeocha unit suites: **9 passing**
- GC stress → `14999850000`
- regex engine, value printer, backtraces, `-m32` output: byte-identical to the
  prior build
- bytecode compile→load round-trip; syntax errors propagate through the new
  `try`/`catch` parse harness (verified past the panic-frame limit)
- CLI builtins (print of every value kind, `Date`, `performance.now`, `load`)
  all correct
