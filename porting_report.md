# MicroQuickJS → Aether porting report

Status of the port of MicroQuickJS to [Aether](../aether/LLM.md), built with
[aeb](../aeb/) and tested partly with [aeocha](../aeocha/).

**The engine is fully ported.** `mquickjs.c` went from **18,366 → 5,002 lines
(−72.8%)**. Everything still in C is the deliberate remainder — the
"third-party-ish things" (math, float conversion, the regex runtime engine,
varargs printf, host cross-compilation tooling, OS/terminal glue) — plus the C
scaffolding the Aether translation units link against.

The whole thing is split across **210 Aether modules** under `ae/`. Every
change kept the `aeb .tests.ae` conformance gate at 7/7, the aeocha unit
suites green, and (for build-tool work) the generated `mquickjs_atom.h` /
`mqjs_stdlib.h` byte-identical.

## Remaining C by file

| File | Lines | What it is |
|---|---:|---|
| **mquickjs.c** | 5,002 | engine — but **only the deliberate remainder** (below); 416 of those lines are `/* ae/ */` forward-declarations |
| **libm.c** | 2,260 | third-party math library |
| **dtoa.c** | 1,618 | third-party float↔string conversion (Bellard's `mp_*`) |
| **mquickjs_build.c** | 869 | host stdlib-baking tool (pure logic ported to Aether; printf emission stays C) |
| **mqjs.c** | 775 | CLI host (gc/REPL helpers ported to Aether; OS/printf stays C) |
| **readline.c** | 742 | terminal line editor (third-party) |
| **mqjs_stdlib.c** | 407 | the stdlib *definition* — DSL data fed to the build tool |
| **example.c** | 308 | embedding demo |
| **readline_tty.c** | 246 | terminal raw-mode glue |
| **example_stdlib.c** | 36 | demo's custom-class stdlib definition |
| **total** | **12,263** | |

## Inside mquickjs.c (5,002 lines) — what is still C

| Category | ~lines | Why it stays C |
|---|---:|---|
| **lre_exec + js_compile_regexp** | 581 | the regex *runtime engine* (third-party-like) |
| **debug dumpers** (`JS_PrintValue*`, `js_dump_*`, `JS_DumpMemory`, `lre_dump_bytecode`, `dump_*`) | ~847 | printf-bound diagnostics |
| **host 64→32 bytecode tools** (`JS_PrepareBytecode*`, `gc_compact_heap_64to32`, `convert_mblock_64to32`, `JS_RelocateBytecode2`, `JS_LoadBytecode`, `expand_short_floats`) | 390 | host cross-compilation tooling |
| **printf / va_list family** (`js_vprintf`, `js_vsnprintf`, `js_snprintf`, `pad`, `is_digit`, `JS_ThrowError`) | 237 | varargs cannot cross into Aether |
| **setjmp parse harness** (`JS_Parse2`, `js_parse_json`, `build_backtrace`) | 162 | setjmp/longjmp + cprintf |
| **dtoa-facing** (`js_dtoa2`, `js_atod1`) | 71 | thin wrappers over dtoa.c |
| **printf shims** (the `js_throw_*` / `js_parse_error_*` carriers extracted during the port) | 69 | intentional `%"JSValue_PRI"` format carriers |
| **VM trampolines** (`vm_call_cfunc_*`, `vm_i2d/u2d/l2d`, `vm_to_number`, `JS_Call`→`js_call_ae`) | 39 | fn-pointer + int→double bridges Aether cannot express |
| **context init** (`JS_NewContext`, `dummy_write_func`) | 8 | printf wrapper |
| **C-data accessors** (`get_op_count`, opcode / char-range table getters) | 7 | expose static-const C tables to Aether |

The rest of the 5,002 lines is **scaffolding, not portable logic**:

- 416 `/* ae/ */` forward-declarations (the bare-symbol contract between C and
  the Aether translation units);
- `static inline` / macro helpers (`JS_NewShortInt`, `hash_prop`,
  `utf8_char_len`, `is_ident_*`, `get_byte_code`, `JS_NewTailCall`, `rotl64`,
  …);
- struct / enum / macro definitions, `#include`s, and comments.

## Why these specific things stay C

The cut line is the boundary where the engine meets things Aether
deliberately cannot or should not express:

- **varargs / printf** — Aether cannot define a `va_list` function. All the
  `%d`/`%s`/`%"JSValue_PRI"` formatting lives in tiny C functions. Where a
  ported Aether function needed to throw a formatted error, the format call
  was extracted into a one-line C "printf shim" (e.g.
  `js_throw_circular_ref`, `js_parse_error_func_name`) that the Aether code
  calls — the logic is Aether, only the format string is C.
- **function-pointer + int→double trampolines** — Aether cannot cast/call a
  raw function pointer it holds as an integer, nor express the C
  integer→double conversions the VM needs. These are kept as a handful of
  tiny C bridges (`vm_call_cfunc_*`, `vm_i2d`, …).
- **setjmp/longjmp** — the parse error harness (`JS_Parse2`) and `build_backtrace`
  use setjmp and cprintf and stay C.
- **third-party / host tooling** — `dtoa.c` (`mp_*`), `libm.c`, the `lre_exec`
  regex runtime engine, the host 64→32 bytecode cross-compilation tools, and
  the readline/terminal code are all "third-party-ish things" outside the
  engine proper, like `dtoa.c` in the original goal.
- **debug dumpers** — `JS_PrintValueF` / `js_dump_*` / `JS_DumpMemory` are
  printf-bound diagnostics, not on the execution path.

## Build tool and CLI

These are host machinery, not the engine. The **pure logic** of each was
ported to Aether; the printf C-text emission and OS syscalls stay C.

- **mquickjs_build.c** (936 → 869): the predicates and atom/cfunc list
  management moved to `gen/buildtool/bt_predicates.ae` and
  `gen/buildtool/bt_atomlist.ae`, linked into the codegen tool via
  `c.aether_source`. Gated on the generated headers staying byte-identical.
- **mqjs.c** (795 → 775): the `gc()` builtin and the REPL syntax-highlighter
  helpers (`is_word`, `find_keyword`) moved to `ae/cli_host.ae`. The rest is
  file I/O, terminal handling, readline, timers, `main()`, and the debug
  dumper — intrinsic host shim.

## Polish (constant deduplication)

`OP_*` and `REOP_*` opcode values were duplicated across many codegen,
parser, and regex modules. They are now sourced from two shared modules —
`ae/opcodes.ae` and `ae/reopcodes.ae` — whose values were each verified
against a C probe over the generated opcode table, with **31 files** migrated
to alias from them. The VM's dispatch table in `ae/vm.ae` is left as the one
canonical place that defines the full opcode set.

`js_`/`JS_` prefixes were **not** stripped: those identifiers are the bare C
ABI symbols that the remaining C calls by name, so renaming them would break
the C↔Aether linkage that the whole port depends on. Prefix-stripping is only
possible once no C remains — and C remains by design.

## Verification

- `aeb .tests.ae` conformance gate: **7/7** (4 JS suites + bytecode roundtrip + embed)
- aeocha unit suites: **9 passing**
- GC stress (`for 100000 {var o={a:i,b:i*2}; s+=o.a+o.b}`) prints `14999850000`
- generated `mquickjs_atom.h` / `mqjs_stdlib.h`: byte-identical to the
  pre-build-tool-port baseline
- broad smoke tests (functions, closures, regex with all flags, arithmetic,
  lvalue/compound-assign codegen, error stack line:col) all correct

## Known straggler

`push_break_entry` (~26 lines in mquickjs.c) is genuinely portable — its
sibling `pop_break_entry` is already Aether. It was left because it is
interleaved with the `BlockEnv` value-stack machinery; it is the one clear
remaining engine leaf if a fully-zero-engine-logic-in-C target is wanted.
