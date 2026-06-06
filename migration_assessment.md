# MicroQuickJS → Aether: Porting Guide & Migration Assessment

**Purpose.** This document is a reproducible recipe for porting Bellard &
Gordon's [MicroQuickJS](https://github.com/bellard/mquickjs) (an ES5-subset
embedded JS engine, MIT-licensed) from C to [Aether](../aether/LLM.md), from
the *same starting point* (Bellard's C tree) to the *same ending point* (this
repo's `main`: a pure-Aether engine built with `aeb`, tested via the
`c.tests` runner). It is written so a second team could repeat the effort and
arrive at the same outcome. **The order of work below is illustrative, not
prescriptive — only the end state matters.** Where the historical journey
taught a lesson, that lesson is captured as an idiom, not a sequence step.

> **Note on `ae/PORT_STATUS.md`.** That file documents *phase 1* (the leaf
> port) and says phase 2 was "not started." It is **stale**. The engine was
> in fact fully ported — `mquickjs.c` was deleted entirely; all engine logic
> is Aether. Trust this document and `git log` over `PORT_STATUS.md`.

---

## 1. Endpoints

### 1.1 Starting point (Bellard's tree)

The base is Bellard's MicroQuickJS at the last upstream commit before porting
began. In this repo that is the 45-commit prefix ending at
`7ea5399 "fixed exception handling in String.prototype.toLowerCase/toUpperCase"`.
The first port commit is `c8659eb`. Reconstruct the boundary with:

```sh
git log --oneline --reverse | head -45      # Bellard's upstream
git log --oneline c8659eb^..HEAD            # the 278 port commits
```

The starting tree is conventional C:

| File | Role |
|---|---|
| `mquickjs.c` (~20 kloc) | the engine: VM dispatch, parser, codegen, builtins, GC, runtime |
| `mquickjs.h`, `mquickjs_priv.h` | public + private struct/API headers |
| `dtoa.c` / `dtoa.h` | float↔string (multi-precision `mp_*`/`mpb_*`) |
| `libm.c` / `libm.h` | IEEE-754 math runtime |
| `cutils.c` / `cutils.h` | string/UTF-8 utilities |
| `mqjs.c` | CLI entry (`main`, REPL, arg parsing) |
| `readline_tty.c` | terminal line editor |
| `mquickjs_build.c` | host-side tool that emits the stdlib ROM table + atom table as C headers |
| `Makefile` | the build |

### 1.2 Ending point (this repo, `main`)

* **Zero engine C.** `mquickjs.c` and `cutils.c` are **deleted**. ~47.5 kloc
  of Aether lives in `ae/` (≈186 `.ae` files) + `gen/`.
* **Irreducible C remainder** (1,863 lines total), kept deliberately:
  * `dtoa.c` (503 ln) — keeps only the `static inline` multi-precision
    `mp_*`/`mpb_*` helpers ("stays C by decree"); the engine-facing
    `js_dtoa` / `output_digits` / `js_atod` bodies moved to `ae/js_dtoa.ae`
    et al. (the C source carries `/* body is now in ae/... */` markers).
  * `libm.c` (649 ln) — math runtime; most functions delegate to libc, kept
    C to match upstream's `js_sqrt` pattern.
  * `mqjs.c` (178 ln) — `main()` is now a 1-line shim: `return
    mqjs_main_run(argc, argv);` into Aether.
  * `readline_tty.c` (246 ln) — low-level TTY ioctl glue.
  * `mqjs_dsl_glue.c` (251 ln), `dsl_demo_glue.c` (36 ln) — host-side
    data structures for the builder-DSL embedding demo.
* **Build is `aeb`**, not `make`: `.build.ae` (mqjs binary),
  `example-app/.build.ae` (embedding demo), `gen/.build.ae` (generated
  headers). The Makefile remains for reference but the canonical build is aeb.
* **Tests run via the `c.tests` runner** in `.tests.ae`.
* **The C codegen tool `mquickjs_build.c` is deleted**, replaced by a
  declarative Aether stdlib DSL (`gen/genengine/`).
* **ABI-stable engine API**: exported symbols are bare (`JS_NewContext`,
  `js_array_push`, …) — the `mqjs_` prefixes used during the port were
  stripped in the final refactors.

The conformance oracle throughout: `test_closure.js`, `test_language.js`,
`test_loop.js`, `test_builtin.js`, a bytecode write/read round-trip, and the
`example`/`test_rect.js` embedding check. **Every port commit kept this green.**

---

## 2. Subsystems to port

This section describes *what* has to be ported, grouped by subsystem, with no
implied ordering. (For the question of sequencing — and why the historical
order is **not** recommended — see §2b.) The point here is the inventory and
the shape of each piece, so a repeat effort knows the territory.

* **Utilities & math.** `cutils.c` (string/UTF-8), `libm.c` (IEEE-754; most
  delegate to libc), and the `dtoa.c` `mp_*`/`mpb_*` multi-precision layer plus
  the engine-facing `js_dtoa`/`js_atod`/`output_digits`. Self-contained, no
  engine state.

* **Stateless engine helpers.** Byte/character predicates, bitstream + pc2line
  decoders, JSValue tag math (→ §5), GC bitfield packing, regexp helpers,
  property-hash, sort callbacks. Pure functions over scalars and `ptr`.

* **Builtins & big algorithms.** `Array.*`, `String.*`, `Object.*`,
  `JSON.stringify`, the regex parser/tokenizer, typed arrays. Each is a
  coherent 50–500-line block that is large but largely self-contained.

* **Parser.** Recursive-descent: `next_token`, `js_parse_statement`, the
  expression ladder, `js_parse_function_decl`, the JSON value parser, the regex
  disjunction/alternative parser.

* **Bytecode VM.** The `JS_Call` interpreter — ported opcode-cluster by
  opcode-cluster until the entire C `switch` is gone and only a thin forwarder
  remains.

* **Engine-state types.** `JSContext`, `JSParseState`, `JSObject` (+ its
  union variants), `JSString`, `JSFunctionBytecode`, `JSValueArray`,
  `JSByteArray`, `JSProperty`, `JSVarRef`, `JSRegExp`, etc. — modelled as
  `extern struct` overlays (→ §4) so engine code can touch fields directly
  instead of via accessor shims.

* **Host & CLI.** `mqjs.c` (`main`, REPL, arg parsing — ends as a 1-line shim
  into Aether), the host glue (I/O, time, `setTimeout` timer list), and the
  embedding `example` app.

* **Build tooling.** Replace the C codegen tool `mquickjs_build.c` with the
  declarative Aether stdlib DSL (→ §9); move the build from `make` to `aeb`
  (→ §8).

**Idioms that paid off (independent of sequencing):**

* **Baby commits.** One logical replacement per commit; the conformance suite
  green at every commit. Small steps are cheap to bisect and revert — keep
  them small regardless of which strategy you pick.
* **Diff-harness for bit-twiddling.** When porting subtle bit-math, keep the C
  body under a `_c` suffix, run both implementations on every call,
  stderr-print divergence, run the full suite + stress, then delete the C body.
  This caught a real `JSString.len` bit-offset bug (it also validated
  `rotl64`, `get_mblock_size`, `mpb_shr_round`, `mpb_mul1_base`,
  `re_need_check_adv_and_capture_init`).

---

## 2b. Sequencing — a note, not a prescription

> **The historical order is not recommended.** This port was driven
> bottom-up: rank functions *most-depended-on / least-depending* (high
> fan-in, low fan-out leaves first) and replace them one at a time, ~300 baby
> commits, with C continuing to call into the freshly-ported Aether via
> generated shims. It worked — the suite stayed green throughout — but it was
> the wrong call. It front-loaded hundreds of throwaway accessor/forwarder
> shims (later demolished wholesale in the overlay sweep), produced Aether
> that read like instrumented C rather than idiomatic Aether until very late,
> and the dependency-ranking machinery was effort spent on ordering rather
> than on the port itself. **Do not repeat the leaf-first, fan-in-ranked,
> C-calls-Aether-via-shim approach just because it is what happened here.**

A better strategy is likely **outside-in**: start at the entry points
(`main`, the embedder API, `JS_Eval`/`JS_Parse2`, the REPL) and work inward,
letting Aether own the outer control flow from the start and pulling C
implementations up into Aether as each call edge is crossed — so Aether calls
*down* into the remaining C rather than C calling *up* into Aether through
shims. The struct-overlay idiom (§4) would then land early as the interface to
engine state, not as a late cleanup. Outside-in is offered here as a
hypothesis worth trying, **not** as a mandate; this assessment deliberately
does not force a sequence. Whatever order is chosen, the invariants are the
same: keep the conformance suite green, and converge on the §1.2 end state.

---

## 3. ABI & type mapping (the core reference)

### 3.1 Scalar type map

| C type | Aether | Notes |
|---|---|---|
| `int`, `int32_t` | `int` | signed 32-bit; exact ABI match |
| `long long`, `int64_t`, `size_t` (64-bit), `JSValue` | `long` | signed 64-bit. Aether's `long` is `long long` at C level — 64-bit on every target incl. MSVC |
| `double` / binary64 | `float` | **Aether `float` lowers to C `double`** (8 bytes). This required a compiler fix (see §7); pre-fix it was 4-byte `float` in extern sigs, an ABI mismatch with libm |
| `void*`, `char*`, `const char*` | `ptr` | opaque; **no `+`/`-`/`[]`/`&` on a bare `ptr` in Aether source** — go through `std.mem` |
| Aether `string` | `{ptr,len}` → `char*` at FFI | length-aware internally; truncates at first NUL when crossing as `char*` |

### 3.2 No unsigned — widen and mask

Aether has no unsigned type. To get C `uint32_t` semantics, widen to `long`
and mask `& 0xffffffff` around any op that needs unsigned interpretation
(`< > / %`). Bit patterns survive across the FFI register boundary; only
*interpretation* is lost.

```aether
// ae/dtoa.ae — u32toa_len: treat n as uint32
long un = n & 0xffffffff
while i >= 0 {
    digit = un % 10        // unsigned division now correct
    un = un / 10
    ...
}
```

Caught early in dtoa (`u32toa` printed `/` instead of `4294967295`). For
genuine 64/32 unsigned division where bit 63 is set, signed `long / int` is
wrong — use `mem.udiv64_32(a, b, rem_slot)` (Möller–Granlund).

### 3.3 No `&local` — malloc the out-param slot

C's `size_t clen; foo(&clen)` has no Aether equivalent. Route the out-slot
through `malloc`/`free` per call:

```aether
// ae/cutils.ae — utf8_get_with_len
slot = malloc(8)
int c = __utf8_get(p, slot)
if p_clen != null { mem.set_int(p_clen, 0, mem.get_long(slot, 0)) }
free(slot)
return c
```

This is a real per-call heap round-trip cost. It is the single biggest
remaining perf debt; a future `alloca`-style stack-slot primitive (or `&x` →
`&x_local` lowering) would erase it.

### 3.4 `std.mem` — the memory-access vocabulary

A bare `ptr` cannot be dereferenced or indexed in Aether source, so all raw
memory access goes through `std.mem` (defined in the Aether tree at
`std/mem/module.ae` + `std/mem/aether_mem.c`). This is the irreducible
C-interop primitive set; the port added several of these primitives upstream
during the work.

| Primitive | C equivalent |
|---|---|
| `mem.get_byte(p,i)` / `mem.set_byte(p,i,c)` | `*(uint8_t*)(p+i)` |
| `mem.get_int` / `mem.set_int` | `*(int32_t*)(p+off)` |
| `mem.get_long` / `mem.set_long` | `*(int64_t*)(p+off)` |
| `mem.get_ptr` / `mem.set_ptr` | `*(void**)(p+off)` |
| `mem.get_float64` / `mem.set_float64` | `*(double*)(p+off)` |
| `mem.bits_of_float` / `mem.float_from_bits` | IEEE-754 reinterpret |
| `mem.clz32` / `mem.clz64` | `__builtin_clz` / `clzll` |
| `mem.udiv64_32(a,b,rem*)` | unsigned `(uint64_t)a / (uint32_t)b`, remainder via slot |
| `mem.ptr_to_long` / `mem.long_to_ptr` | `(uintptr_t)p` / `(void*)(uintptr_t)v` |
| `mem.call_fn3_int` / `mem.call_fn3_void` | call a **bare C fnptr** `f(a,b,opaque)` |

**`call_fn3_*` is the fnptr bridge.** Aether's `fn` type lowers to a closure
struct `{fn_ptr, env}`, incompatible with C's bare function-pointer ABI. C
callbacks (the `rqsort_idx` comparator/swap, sort callbacks) are passed as
`ptr` and invoked through these shims:

```aether
// ae/mquickjs.ae — rqsort_idx heapsort driver
if mem.call_fn3_int(cmp, c, c + 1, opaque) <= 0 { c = c + 1 }
...
mem.call_fn3_void(swap, r, c, opaque)
```

### 3.5 `extern` declarations & variadic externs

Calling C from Aether uses `extern`:

```aether
extern strlen(s: ptr) -> int
extern malloc(n: int) -> ptr
extern JS_Call(ctx: ptr, call_flags: int) -> long
```

**Variadic externs** replace the swarm of fixed-message C throw shims. Before,
each error message needed its own C wrapper; after, declare the C variadic
directly:

```aether
extern JS_ThrowTypeError(ctx: ptr, fmt: ptr, ...) -> long
extern JS_ThrowInternalError(ctx: ptr, fmt: ptr, ...) -> long
// call site:
JS_ThrowTypeError(ctx, "invalid array length")
JS_ThrowInternalError(ctx, "bytecode not saved for %d-bit", jsw() * 8)
```

This idiom retired **104** `_str`-suffix shims plus the per-message
`mqjs_throw_*` family. (Requires the Aether toolchain's variadic-extern
support; it was added around `aetherc` v0.171.)

---

## 3a. Aether features the port actually used

The full inventory of Aether language and library surface this port touched —
useful both as a "what you need from the toolchain" checklist and as a measure
of how small that surface is. Counts are from the `.ae` sources on `main`; they
are call-site / occurrence counts, indicative not exact.

### Language features

| Feature | Used for | Scale |
|---|---|---|
| `extern fn(...) -> t` | calling C (libc, engine internals) | pervasive |
| **variadic** `extern f(..., ...) -> t` | direct `JS_Throw*Error`/`printf`-family calls, no per-message shim (§3.5) | ~72 decls |
| `extern struct { … }` with **bitfields** (`f: t : width`) and **flexible-array tails** (`buf: byte[]`, `arr: long[0]`) | mirroring C engine structs (§4) | 26 distinct structs |
| **pointer cast** `(p as *Type)` then `.field` read/write | overlay field access (§4) | ~1,360 sites |
| **fn-pointer cast** `x as fn(a,b)->r` | bridging C bare fnptrs to `call_fn3_*` shims (§3.4) | ~27 sites |
| `const` declarations | tags, opcodes, atom offsets, struct offsets, flag masks | ~1,500 decls |
| `try { } catch e { }` + `panic` | replacing `setjmp`/`longjmp` parser error escape (§6a) | 2 try-sites, ~74 panics |
| `if`/`else`, `while`, `for`, `break`, `continue`, `return` | all control flow (no `goto`, §6) | pervasive |
| typed locals (`int`, `long`, `float`, `ptr`) + bare-name assignment | everywhere | pervasive |
| **trailing-closure builder DSL** (`name(args) { …body… }`) | the stdlib spec and the `mqjs`/`run() { }` embedding DSL (§9) | ~77 blocks |
| module system: `import mod`, selective `import mod (a, b)`, local modules (`ae/<name>/module.ae`) | code organization | pervasive |

> **Notably *not* used** (so a repeat port need not depend on them): the actor
> model (`send`/`receive`/`spawn`), tuple / `(value, err)` multi-return,
> pattern `match`, string interpolation (used in ≤3 trivial spots), `*StringSeq`
> cons-lists, glob imports `(*)`, and the `hide`/`seal except`/`--with=`
> sandbox machinery. This is a *systems-FFI* slice of Aether: externs, struct
> overlays, raw memory, and manual control flow — not the high-level surface.

### Standard-library surface

| Module | Functions used | Role |
|---|---|---|
| `std.mem` | `get_/set_{byte,int,long,ptr,float64,float32,uint8,uint16,uint32}`, `get_/set_u{16,32}_le`, `get_int8/int16`, `ptr_to_long`/`long_to_ptr`, `bits_of_float`/`float_from_bits`, `clz32`/`clz64`, `udiv64_32`, `call_fn3_int`/`call_fn3_void`/`call_fn2_void`, `copy` | **the** interop backbone — all raw memory access and fnptr calls (§3.4); ~4,000 call sites |
| `std.string` | `concat` (~324×), `from_char`, `len` | building C-string literals for emitters/messages |
| `std.strbuilder` | `new`, `append_format`, `finish` | the stdlib-header emitter's text sink (§9); ~100 sites |
| `std.os` | `wall_seconds`, `wall_micros` | host time for `setTimeout`/`performance.now` |
| `std.io` | `write_file` | writing generated headers in `gen/.build.ae` |
| `std.path` | `join` | build-output paths in the gen step |
| `std.list` | `new`, `next` | the stdlib-spec builder's backing list (§9) |

### Build / toolchain surface (aeb, not the runtime)

`aeb` build DSL (§8): `build.start`/`dep`/`mkdirs`/`target_dir`/`_write_artifact`;
`c.program`/`c.tests`/`run`/`sources`/`aether_source`/`cflag`/`link_flag`/
`output_file`/`aether_home`/`aether_caps`; `aether.program`/`source`/`output`/
`extra_source`/`regen`/`include_dir`. Plus `ae cflags` for any external
embedder's link line.

The takeaway: porting a 20-kloc C engine exercised a deliberately narrow
Aether vocabulary. `std.mem` + `extern struct` overlays do the overwhelming
majority of the work; everything else is a thin rim.

---

## 4. The `extern struct` overlay idiom (the heart of the port)

This is what made an in-place port of engine-state code possible. Instead of
hundreds of C accessor shims (`mqjs_jsstring_len(p)`, `mqjs_jsobject_set_proto(p,v)`,
…), declare an Aether `extern struct` that **byte-for-byte mirrors the C
struct layout**, then read/write fields directly with `(ptr as *Type).field`.
The overlays live in `ae/mqtypes/module.ae`.

### 4.1 Worked example — `JSString`

**C** (`mquickjs_priv.h`, mirrored in the layout-guard gen):

```c
typedef struct JSString {
    uint64_t gc_mark : 1;
    uint64_t mtag : 3;
    uint64_t is_unique : 1;
    uint64_t is_ascii : 1;
    uint64_t is_numeric : 1;
    uint64_t len : 57;
    unsigned char buf[];   // flexible array
} JSString;
```

**Aether** (`ae/mqtypes/module.ae`):

```aether
extern struct JSString {
    gc_mark:    uint64 : 1     // bit 0
    mtag:       uint64 : 3     // bits 1-3
    is_unique:  uint64 : 1     // bit 4
    is_ascii:   uint64 : 1     // bit 5
    is_numeric: uint64 : 1     // bit 6
    len:        uint64 : 57    // bits 7-63
    buf:        byte[]         // flexible-array tail @ offset 8
}
```

**Call site:**

```aether
len2     = (p2 as *JSString).len
is_ascii = (p2 as *JSString).is_ascii
src      = (p1 as *JSString).buf + 0
```

### 4.2 Syntax rules

* **Bitfields:** `field: type : width` — e.g. `magic: int : 16` for a C
  `int16_t`, `gc_mark: uint64 : 1`. Bit-widths and field order must match the
  C side exactly; gcc bitfield packing is assumed (the engine's only real
  target is 64-bit/`JSW=8`).
* **Flexible-array tail:** `buf: byte[]` or `arr: long[0]`. Element indexing on
  the tail is not yet exposed on extern structs, so reach elements via base +
  offset arithmetic: `(p as *JSString).buf + i`.
* **Unions:** union members are *not* declared inline. Define an offset const
  and cast at it; the active variant is chosen by `class_id`:

  ```aether
  const JSOBJ_U_OFFSET = 24
  len = ((obj + mqtypes.JSOBJ_U_OFFSET) as *JSArrayData).len
  ```
* **Explicit padding:** insert `pad_: int` / `alloc_pad_: int` fields where C
  alignment inserts gaps, so offsets stay correct.
* **Reserved-word renames:** Aether reserves `func`, `message`, `state`,
  `match`, `after`. Rename the field (`func_`, `message_`) — the byte offset is
  what matters; the emitted C uses the renamed identifier and offsets agree.
* **Embedded structs:** model with an offset const (`JSTIMER_FUNC_OFFSET = 8`)
  when code needs a *handle* to the sub-struct as a GC root.

### 4.3 Direct field access vs. accessor calls

With an overlay in place, engine code reads and writes fields inline. The
alternative — a C accessor function per field (`mqjs_jstimer_allocated(p)`,
`mqjs_jstimer_set_timeout(p, v)`, …) — is what this port accreted early and
removed later; under outside-in (§2b) you would write the overlay form from
the start and never grow the accessors at all.

```aether
// accessor-call form (avoid): one C function per field, an FFI hop each
if mqjs_jstimer_allocated(th) == 0 {
    mqjs_jstimer_set_timeout(th, now + delay)
    mqjs_jstimer_set_allocated(th, 1)
}
// overlay form (target): direct field access, no shim
if (th as *JSTimer).allocated == 0 {
    (th as *JSTimer).timeout = now + delay
    (th as *JSTimer).allocated = 1
}
```

The overlays cover the whole engine: `JSObject` head + union variants (7
overlays), `JSFunctionBytecode`, `JSString` head + flexible `buf` tail,
`JSParseState`, `JSContext`, `JSValueArray`, `JSByteArray`, `JSProperty`,
`JSVarRef`, plus the JSValue tag helpers and the `re_sp_*` stack-pointer
helpers. The payoff is the same whenever the overlay is introduced: field
access is a native load/store, not an FFI round-trip.

### 4.4 Layout drift guards

Because the overlay's correctness depends on offsets matching, the port keeps
**layout-guard** `_Static_assert`s on the C side (`ae/layout_guard.ae` +
generated `layout_guard_gen.c`) asserting `sizeof`/`offsetof` for each mirrored
struct. If a C struct ever changes shape, the build fails loudly rather than
the engine corrupting memory silently. **Reproduce this — it is the safety net
for the entire overlay strategy.**

---

## 5. JSValue tag encoding (`ae/jstag/module.ae`)

`JSValue` is a tagged `uint64` (Aether `long`). Tag bits live low; payload
high. The C macros became native Aether bit-math (563 call sites):

```
JS_TAG_INT      = 0    (low bit 0)
JS_TAG_PTR      = 1    (low bits 01)
JS_TAG_SPECIAL  = 3    (low bits 11; bool/null/undefined/exception/… extend to 5 bits)
```

```aether
is_int(v: long)  -> int { if (v & 1) == TAG_INT { return 1 } return 0 }
is_ptr(v: long)  -> int { if (v & 7) == TAG_PTR { return 1 } return 0 }
new_short_int(v: int) -> long { long lv = v   return lv << 1 }
value_to_ptr(v: long)  -> ptr  { return mem.long_to_ptr(v - 1) }   // strip tag
value_from_ptr(p: ptr) -> long { return mem.ptr_to_long(p) + 1 }   // add tag
```

Keeping these in one module (rather than inlining the bit-math at each of the
hundreds of use sites) is what makes tag handling uniform and easy to get
right across the engine.

---

## 6. Control-flow porting (no `goto`, no `setjmp`)

C uses `goto fail`/`goto done` and `setjmp`/`longjmp`. Aether has neither.
Three idioms cover every case:

**(a) `try`/`catch` + `panic` replaces `setjmp`/`longjmp`.** The parser's
error escape became an Aether try/catch; the C side keeps only the thin shell.

```aether
// ae/parse2.ae — JS_Parse2 error boundary
try {
    result = JS_Parse2_run(ctx, ps, filename, eval_flags)
} catch reason {
    jsps_set_error_msg(ps, reason)
    is_error = 1
}
if is_error != 0 { return JS_Parse2_on_error(...) }
```

**(b) Flag-driven loop replaces `goto redo`.** The lexer's restart-on-comment
becomes an outer `while looping` with `looping = 0` as the break:

```aether
// ae/next_token.ae
int looping = 1
while looping != 0 {
    int c = lex_get(p, 0)
    if c == 0 { ...; looping = 0 }
    else if c == 10 { got_lf = 1; p = p + 1 }      // '\n' — fall through to redo
    else if c == 47 { /* comment: scan, then loop again */ }
    else { p = lex_def_token(s, p, c); looping = 0 }
}
```

**(c) Explicit state-machine for goto-based recursive descent ("parser-resume").**
The expression ladder (`js_parse_logical_and_or`, etc.) can't keep C locals
across recursive calls without `goto`. Each function takes a state `int`,
returns an encoded `state | (func<<8) | (param<<16)` token telling a driver
which sub-parser to call next, and saves/restores locals on a runtime int
stack (`js_parse_push_val`/`js_parse_pop_val`). `PARSE_STATE_RET` (0xff) signals
return. Verbose but mechanical; see `ae/parse_logical.ae`.

**(d) `handled` flag replaces `switch` fall-through** in cascades of `if
handled == 0 && dt == DT_X` (see the stdlib emitter).

---

## 7. Aether-side changes required by the port

Four compiler fixes and a set of `std.mem` primitives were upstreamed into the
Aether tree (originally on `feat/mquickjs-interop`). A repeat port needs an
Aether toolchain that already has these:

**Compiler fixes:**
1. **`TYPE_FLOAT` → C `double` everywhere.** `float` previously lowered to
   4-byte C `float` in extern signatures but 8-byte in locals — an ABI
   mismatch against libm's `double`. Fixed in `codegen/codegen.c`.
2. **`ptr + int` / `ptr - int` must stay `ptr`.** The typechecker truncated
   pointer arithmetic to 32-bit `int`, corrupting 47-bit heap addresses.
   Fixed so `+`/`-` yield `TYPE_PTR` (`*`/`/`/`%` keep legacy boxed-int).
3. **Hex/octal/binary literals ≥ 2^63.** Lexer used signed `strtol`, clamping
   `0x8000000000000000` to `LONG_MAX`. Switched to `strtoull` + `(int64_t)…ULL`.
4. **Struct-literal-in-`if`-condition.** `if a == b {}` parsed `b {}` as an
   empty struct literal, eating the body. Fixed to respect `in_condition`.

**`std.mem` additions:** the byte/int/long/ptr/float-bits accessors, `clz32/64`,
`udiv64_32`, and the `call_fn3_int/void` fnptr shims (§3.4). Probed under
`tests/integration/std_mem_byte_access/`.

**Toolchain features relied on later:** variadic `extern` (§3.5), `extern
struct` overlays with bitfields/flexible-arrays (§4), `try`/`catch`/`panic`
(§6a), the `aeb` build system, and `ae cflags` for the embedder link line.

---

## 8. Build system: `make` → `aeb`

The Makefile is replaced by three aeb scripts. aeb (the multi-package build
system, repo `aether-lang-org/aeb`) reads `share/aether/MANIFEST` to discover
link-suitable runtime/stdlib `.c` and orchestrates per-package compile + cache
+ incremental relink.

### 8.1 `gen/.build.ae` — generated headers

Materializes the stdlib spec into `mqjs_stdlib.h` + `mquickjs_atom.h` (both
gitignored) under `target/<node>/include`, then publishes them to consumers via
the `c_header_dirs` artifact:

```aether
global_obj = genengine.build_global(genengine.mqjs_stdlib_spec())
io.write_file(path.join(inc, "mqjs_stdlib.h"),
              genengine.stdlib_table(global_obj, cdecl_obj))
io.write_file(path.join(inc, "mquickjs_atom.h"), genengine.atom_defines())
build._write_artifact(b, "c_header_dirs", inc)
```

### 8.2 `.build.ae` — the `mqjs` binary (`c.program`)

```aether
c.program(b) {
    cflag("-Os"); cflag("-D_GNU_SOURCE")
    cflag("-fno-math-errno"); cflag("-fno-trapping-math"); cflag("-I.")
    aether_home("/home/paul/scm/aether")   // auto -I's every runtime/ + std/ subdir
    aether_caps("os,fs,net")               // mqjs IS the host → grant gated caps
    mqjssources.register_engine_c_sources(".")   // dtoa.c, libm.c
    mqjssources.register_engine_sources("ae")    // the ~160 engine .ae files
    sources("mqjs.c"); sources("readline_tty.c") // CLI front-end (C)
    aether_source("ae/mqjs.ae"); aether_source("ae/mqjs_glue.ae")
    aether_source("ae/readline.ae"); aether_source("ae/host_state.ae")
    link_flag("-lpthread"); link_flag("-ldl"); link_flag("-lm")
    output_file("mqjs")
}
```

Key knobs: `aether_home(path)` points at the Aether dev checkout and auto-adds
its `runtime/`+`std/` subdirs as include paths; `aether_caps("os,fs,net")`
grants the gated stdlib capabilities (correct because mqjs *is* the host, not
untrusted guest code — see the `--emit=lib` capability discussion in Aether's
`docs/emit-lib.md`); `cflag`/`link_flag` pass through to the C
compiler/linker; `output_file` names the binary.

### 8.3 `example-app/.build.ae` — the embedding demo (`aether.program`)

An `aether.program` where `ae/example.ae`'s `main()` *is* the process entry
(aetherc's default exe emit gives `main(argc,argv)` → `aether_args_init` →
your `main()`), with **zero committed C main shim**. It re-uses the same engine
source set via `register_engine_regen` so the two binaries can't drift.

### 8.4 Single source of truth: `gen/mqjssources`

Both binaries list the engine sources *once*, in
`gen/mqjssources/module.ae`:

```aether
register_engine_c_sources(_ctx: ptr, root: string) {
    sources(string.concat(root, "/dtoa.c"))   // stays C by decree
    sources(string.concat(root, "/libm.c"))
}
register_engine_sources(_ctx: ptr, prefix: string) {
    aether_source(string.concat(prefix, "/cutils.ae"))
    aether_source(string.concat(prefix, "/libm.ae"))
    ...                                         // ~160 entries
}
```

`.build.ae` calls `register_engine_sources("ae")`; `example-app/.build.ae`
calls `register_engine_regen("../ae")`. **Reproduce this single-list pattern** —
it is the thing that prevents the two embedders from drifting.

---

## 9. The stdlib DSL (replacing `mquickjs_build.c`)

Upstream generates the builtin ROM (atom table, class defs, property defs) with
a host-side C tool, `mquickjs_build.c`. The port replaced it with a declarative
Aether DSL in `gen/genengine/`, then **deleted the C tool**. The spec reads
like a config file; the emitter materializes it into the same C ROM tables
upstream produced.

```aether
mqjs_stdlib_spec() -> ptr {
    g = stdlib() {
        klass("Array", 1, "js_array_constructor", "JS_CLASS_ARRAY") {
            statics() { cfunc("isArray", 1, "js_array_isArray") }
            proto() {
                cgetset("length", "js_array_get_length", "js_array_set_length")
                cfunc_magic("push", 1, "js_array_push", "0")
                cfunc_magic("map", 1, "js_array_every", "js_special_map")
            }
        }
        object("Math") {
            statics() { cfunc_magic("min", 2, "js_math_min_max", "0")
                        double("PI", 3.141592653589793) }
        }
        cfunc("parseInt", 2, "js_number_parseInt")
        double("Infinity", 1.0 / 0.0)
    }
    return g
}
```

Builders: `klass`/`object`/`errkind` (with `statics()`/`proto()` blocks),
`cfunc`/`cfunc_magic`/`cgetset`/`double`/`extends`/`set_finalizer`. Under the
hood each `sp_*` writer pokes a 48-byte `JSPropDef` record via `mem.set_*`;
`stdlib_table()` emits `js_stdlib_table[]` + `js_c_function_table[]` +
finalizers, and `atom_defines()` emits `#define JS_ATOM_<name> <offset>`. This
is a textbook case of Aether's "config IS code" / trailing-block-DSL pattern.

---

## 10. Test framework

The Makefile's `test` target becomes `.tests.ae`, an aeb test target run with
`aeb .tests.ae`. It `dep`s on both program targets (so they build first), then
declares one `run(...)` per case inside a `c.tests` block:

```aether
c.tests(b) {
    run(".build.ae", "tests/test_closure.js")
    run(".build.ae", "tests/test_language.js")
    run(".build.ae", "tests/test_loop.js")
    run(".build.ae", "tests/test_builtin.js")
    run(".build.ae", "-o test_builtin.bin tests/test_builtin.js")  // bytecode write
    run(".build.ae", "-b test_builtin.bin")                        // bytecode read
    run("example-app/.build.ae", "tests/test_rect.js")             // embedding check
}
```

**Pass/fail model:** each `run` invokes the named target's binary with the
given args; **exit 0 = pass, non-zero = fail**. There is no per-assertion
framework in-tree — the JS test files self-check and `exit(1)` on mismatch, and
the bytecode round-trip passes iff the read-back run exits clean.

> **On "aeocha".** The task brief mentions an *aeocha* test framework. No file
> in this repo references `aeocha`/`aecha`; what is actually present is the
> `c.tests`/`run()` exit-code harness above. If a richer aeocha
> assertion/reporting layer exists in the wider Aether ecosystem, wiring it in
> would be a drop-in replacement for the `c.tests` block — the run-binary,
> check-exit-code contract is the same. Treat this section as the *current*
> reality; upgrade to aeocha if/when it lands.

---

## 11. Persistent gaps (known debt to budget for in a repeat port)

These were worked around, not solved; a repeat effort will hit them too:

* **No unsigned types** → widen-to-`long` + `& 0xffffffff` (§3.2).
* **No `&local`** → `malloc`/`free` per out-param; real per-call heap cost (§3.3).
* **No element indexing on flexible-array tails** → base + offset arithmetic.
* **`mem.*` is a function call** → overhead in the hottest loops (bytecode
  dispatch, UTF-8 decode). Correctness was prioritized; not optimized.
* **Bitfield correctness is offset-fragile** → mitigated by the layout-guard
  `_Static_assert`s (§4.4), but every overlay edit must keep those passing.
* **Platform assumption: 64-bit only.** Overlays hardcode `JSW=8` widths; a
  32-bit target would need a second set of overlay widths.

---

## 12. Reproduction checklist

This is a checklist of *outcomes to reach*, not a strict sequence. It does
**not** prescribe the leaf-first order this port used (see §2b); arrange the
middle steps to suit whatever strategy you adopt.

1. **Prep the toolchain.** Land the four compiler fixes and the `std.mem`
   primitive set in Aether (§7). Confirm `extern struct`, variadic `extern`,
   `try`/`catch`/`panic`, and `aeb` are all available.
2. **Stand up the build with a green gate.** Get a mixed C+Aether build
   producing the engine, with the conformance suite (§1.2 oracle) runnable
   from the first commit. (`aetherc` can be wired into the existing Makefile to
   bootstrap, then migrated to `aeb`; or start on `aeb` directly.)
3. **Model engine-state types as overlays** in `ae/mqtypes/module.ae`, with
   layout-guard asserts (§4). Having these early lets Aether touch engine state
   directly instead of accreting accessor shims — useful under any strategy,
   essential under outside-in.
4. **Port the subsystems** in §2 — utilities/math, stateless helpers, builtins,
   parser, VM, host/CLI — in whatever order keeps the suite green. Use baby
   commits and diff-harness the bit-math (§2 idioms).
5. **Replace `mquickjs_build.c`** with the Aether stdlib DSL (§9); delete the
   C tool.
6. **Reduce the C surface to the deliberate remainder.** Port `mqjs.c`'s
   `main` to a 1-line shim and the host glue to Aether; keep only
   `dtoa.c`/`libm.c`/`readline_tty.c`/the DSL glue as irreducible C.
7. **Delete `mquickjs.c` and `cutils.c`.** Verify zero engine C remains.
8. **Polish:** drop interim symbol prefixes, split grab-bag files into themed
   modules, expand cryptic names, dedup consts.

**Done when:** `aeb .build.ae` and `example-app/.build.ae` build clean,
`aeb .tests.ae` is green (4 JS suites + bytecode round-trip + embedding check),
and the only `.c` files left are the deliberate irreducible remainder in §1.2.
