# MicroQuickJS → Aether porting report

Status of the port of MicroQuickJS to [Aether](../aether/LLM.md), built with
[aeb](../aeb/) and tested partly with [aeocha](../aeocha/).

**The engine is fully ported — every portable function is Aether.** `mquickjs.c`
went from **18,366 → 2,596 lines (−85.9%)**, and what remains is genuinely-C
scaffolding: the setjmp parse harness, static-const data tables, inline-helper
macros, and the forward declarations that *are* the C↔Aether ABI contract.

The engine is split across **224 Aether modules** under `ae/`. Every change
kept the `aeb .tests.ae` conformance gate at 7/7, the aeocha unit suites green,
the GC-stress program correct (`14999850000`), and — where applicable — the
generated headers and the `-m32` / bytecode round-trip output byte-identical to
the previous build.

## What made the final push possible

Two Aether language features (both in the toolchain this port builds against)
dissolved the barriers that had earlier kept whole subsystems in C:

- **Aether-authored C-variadic functions** (`va_start` / `va_arg(vap, T)` /
  `va_end`). An Aether function can own a real C `va_list` and walk it, so the
  printf/`JS_ThrowError` family became Aether — and with it everything that was
  blocked only by needing a formatted throw.
- **Typed C function-pointer dispatch** (`ptr as fn(T1, …) -> R` then call).
  This retired the VM's `vm_call_cfunc_*` trampolines.

With those, the value pretty-printer, `build_backtrace`, the dtoa-facing
wrappers, the regex runtime engine `lre_exec`, and the bytecode relocation /
64→32 cross-compilation tools all became ordinary ports.

## Remaining C by file

| File | Lines | What it is |
|---|---:|---|
| **mquickjs.c** | 2,596 | engine *scaffolding* only — 457 lines are `/* ae/ */` forward declarations; the rest is the setjmp harness, data tables, inline helpers, struct/enum/macro defs, comments |
| **libm.c** | 2,260 | third-party math library |
| **dtoa.c** | 1,618 | third-party float↔string conversion (Bellard's `mp_*` bignum) |
| **mquickjs_build.c** | 869 | host stdlib-baking tool (pure logic ported to Aether; printf emission stays C) |
| **mqjs.c** | 775 | CLI host (gc/REPL helpers ported; OS/printf stays C) |
| **readline.c** | 742 | terminal line editor (third-party) |
| **mqjs_stdlib.c** | 407 | the stdlib *definition* — DSL data |
| **example.c** | 308 | embedding demo |
| **readline_tty.c** | 246 | terminal raw-mode glue |
| **example_stdlib.c** | 36 | demo's custom-class stdlib definition |
| **total** | **9,857** | |

## What is still C in mquickjs.c (and why)

The 2,596 lines are no longer engine *logic* — every real algorithm is Aether.
What remains:

- **The setjmp parse harness** — `JS_Parse2` allocates the `JSParseState`
  struct (with its embedded `jmp_buf`) on the C stack and `setjmp`s it; the
  parse-error path `longjmp`s back to it. `setjmp`/`longjmp` and a C-stack
  struct cannot cross into Aether, so this stays C. The error *formatting* is
  Aether (`js_parse_error` writes the message, then calls the 3-line C
  `js_parse_longjmp`); only the jump itself is C.
- **Static-const data tables + their accessors** — `opcode_info[]`,
  `reopcode_info[]`, `char_range_s/w[]`. These are C data the Aether modules
  read through `get_opcode_info_table()` etc.
- **`static inline` / macro helpers** — `JS_NewShortInt`, `JS_NewTailCall`,
  `hash_prop`, `utf8_char_len`, `is_ident_first/next`, `unicode_is_space`,
  `get_byte_code`, `rotl64`, … (small definitions inlined into both C and the
  layout guard).
- **The `JS_Call` shim** — a one-line forward to the Aether `js_call_ae`.
- **The printf "shims" deliberately kept in C** — none remain; the throw
  helpers became Aether once `JS_ThrowError` did.
- **457 forward declarations** — the bare-symbol contract: each `/* ae/… */`
  line tells C the signature of a function whose body lives in an Aether
  translation unit.
- **struct/enum/macro definitions, `#include`s, comments.**

## Build tool and CLI (host machinery, not the engine)

The **pure logic** of each was ported to Aether; the printf C-text emission and
OS syscalls stay C — these are host machinery, like `dtoa.c`.

- **mquickjs_build.c** (936 → 869): predicates and atom/cfunc list management
  moved to `gen/buildtool/*.ae`, linked into the codegen tool via
  `c.aether_source`, gated on the generated headers staying byte-identical.
- **mqjs.c** (795 → 775): the `gc()` builtin and the REPL syntax-highlighter
  helpers moved to `ae/cli_host.ae`. The rest is file I/O, terminal handling,
  readline, timers, `main()` — intrinsic host shim.

## Verification

- `aeb .tests.ae` conformance gate: **7/7** (4 JS suites + bytecode round-trip + embed)
- aeocha unit suites: **9 passing**
- GC stress (`for 100000 {var o={a:i,b:i*2}; s+=o.a+o.b}`) → `14999850000`
- regex engine: output byte-identical to the prior build; GC-churning regex
  loops correct
- value printer / backtraces: output byte-identical to the prior build
- number formatting/parsing: float precision, toFixed/toExponential/
  toPrecision, radix conversion, parse* whitespace handling — all correct
- bytecode: compile→load round-trip works; `-m32` cross-compiled output
  byte-identical to the prior build across float/unicode/object/regex programs

## Constants polish

`OP_*` and `REOP_*` opcode values were deduplicated into shared
`ae/opcodes.ae` and `ae/reopcodes.ae` modules (values verified against the
opcode-table probe), with 31 consumer modules migrated to alias from them. The
VM's dispatch table in `ae/vm.ae` is left as the one canonical full opcode set.

`js_`/`JS_` prefixes are intentionally **not** stripped: those identifiers are
the bare C ABI symbols the remaining C calls by name, so renaming would break
the C↔Aether linkage.

## Why the remaining C cannot reach zero

`JS_Parse2`'s `setjmp` over a C-stack-allocated parse state is the irreducible
core: Aether has no `setjmp`/`longjmp` and cannot `setjmp` a struct living on
its own stack frame. The data tables and inline helpers could in principle move
to Aether `const` arrays / a header, shrinking the file further, but the file
itself remains as the setjmp shell plus the C↔Aether forward-declaration
contract.
