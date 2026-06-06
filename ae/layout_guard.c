/* Layout guard for the ae/mqtypes overlays. Mirrors the engine struct
 * definitions from mquickjs.c and asserts, at compile time, the
 * sizeof/offsetof the Aether overlays assume. If a C struct ever
 * changes shape the build fails loudly rather than the overlays
 * silently corrupting memory.
 *
 * The struct bodies are copied verbatim from mquickjs.c (they are
 * file-private there, so cannot be #included). Keep in sync; the
 * _Static_asserts are the tripwire that catches drift. */
#include <stddef.h>
#include <stdint.h>

#include "mquickjs.h"   /* JSValue, JSWord, JSW */

#define JS_MTAG_BITS 4
#define JS_MB_PAD(n)  (JSW * 8 - (n))
#define JS_MB_HEADER \
    JSWord gc_mark: 1; \
    JSWord mtag: (JS_MTAG_BITS - 1)

typedef struct {
    JS_MB_HEADER;
    JSWord is_unique: 1;
    JSWord is_ascii: 1;
    JSWord is_numeric: 1;
    JSWord len: JS_MB_PAD(JS_MTAG_BITS + 3);
    uint8_t buf[];
} GuardJSString;

typedef struct {
    JS_MB_HEADER;
    JSWord size: JS_MB_PAD(JS_MTAG_BITS);
    uint8_t buf[];
} GuardJSByteArray;

typedef struct {
    JS_MB_HEADER;
    JSWord size: JS_MB_PAD(JS_MTAG_BITS);
    JSValue arr[];
} GuardJSValueArray;

typedef struct {
    JSValue key;
    JSValue value;
    uint32_t hash_next : 30;
    uint32_t prop_type : 2;
} GuardJSProperty;

typedef struct {
    JSValue func_bytecode;
    JSValue var_refs[];
} GuardJSClosureData;

typedef struct {
    uint32_t idx;
    JSValue params;
} GuardJSCFunctionData;

typedef struct {
    JSValue tab;
    uint32_t len;
} GuardJSArrayData;

typedef struct {
    JSValue message;
    JSValue stack;
} GuardJSErrorData;

typedef struct {
    JSValue byte_buffer;
} GuardJSArrayBuffer;

typedef struct {
    JSValue buffer;
    uint32_t len;
    uint32_t offset;
} GuardJSTypedArray;

typedef struct {
    JSValue source;
    JSValue byte_code;
    int last_index;
} GuardJSRegExp;

typedef struct {
    JSValue proto;
    JSValue props;
    union {
        GuardJSClosureData closure;
        GuardJSCFunctionData cfunc;
        GuardJSArrayData array;
        GuardJSErrorData error;
        GuardJSArrayBuffer array_buffer;
        GuardJSTypedArray typed_array;
        GuardJSRegExp regexp;
        double date;
        void *user;
    } u;
} GuardJSObjectBody;

typedef struct JSFunctionBytecode {
    JS_MB_HEADER;
    JSWord has_arguments : 1;
    JSWord has_local_func_name : 1;
    JSWord has_column : 1;
    JSWord arg_count : 16;
    JSWord dummy: JS_MB_PAD(JS_MTAG_BITS + 3 + 16);
    JSValue func_name;
    JSValue byte_code;
    JSValue cpool;
    JSValue vars;
    JSValue ext_vars;
    uint16_t stack_size;
    uint16_t ext_vars_len;
    JSValue filename;
    JSValue pc2line;
    uint32_t source_pos;
} GuardJSFunctionBytecode;

/* --- the assertions ------------------------------------------------- */

/* JSString: header 8 bytes, buf at offset 8. */
_Static_assert(offsetof(GuardJSString, buf) == 8, "JSString.buf @8");

/* JSByteArray / JSValueArray: header 8 bytes, tail at offset 8. */
_Static_assert(offsetof(GuardJSByteArray, buf) == 8, "JSByteArray.buf @8");
_Static_assert(offsetof(GuardJSValueArray, arr) == 8, "JSValueArray.arr @8");

/* JSProperty: key@0 value@8 bits@16, size 24. */
_Static_assert(offsetof(GuardJSProperty, key) == 0, "JSProperty.key @0");
_Static_assert(offsetof(GuardJSProperty, value) == 8, "JSProperty.value @8");
_Static_assert(sizeof(GuardJSProperty) == 24, "JSProperty size 24");

/* JSObject body: proto@0 props@8 union@16 (within the post-header part).
 * In the real JSObject the header is 8 bytes, so proto is at offset 8,
 * props @16, u @24 — matching JSOBJ_U_OFFSET. We check the body-local
 * offsets here (header excluded) and add 8 for the engine offset. */
_Static_assert(offsetof(GuardJSObjectBody, proto) == 0, "obj.proto body@0");
_Static_assert(offsetof(GuardJSObjectBody, props) == 8, "obj.props body@8");
_Static_assert(offsetof(GuardJSObjectBody, u) == 16, "obj.u body@16");

/* union variant field offsets (relative to u start). */
_Static_assert(offsetof(GuardJSArrayData, tab) == 0, "array.tab @0");
_Static_assert(offsetof(GuardJSArrayData, len) == 8, "array.len @8");
_Static_assert(offsetof(GuardJSCFunctionData, params) == 8, "cfunc.params @8");
_Static_assert(offsetof(GuardJSTypedArray, len) == 8, "ta.len @8");
_Static_assert(offsetof(GuardJSTypedArray, offset) == 12, "ta.offset @12");
_Static_assert(offsetof(GuardJSRegExp, last_index) == 16, "regexp.last_index @16");

/* JSFunctionBytecode: header 8, then JSValues at 8/16/24/32/40,
 * stack_size@48 ext_vars_len@50, filename@56 pc2line@64 source_pos@72. */
_Static_assert(offsetof(GuardJSFunctionBytecode, func_name) == 8, "fb.func_name @8");
_Static_assert(offsetof(GuardJSFunctionBytecode, byte_code) == 16, "fb.byte_code @16");
_Static_assert(offsetof(GuardJSFunctionBytecode, cpool) == 24, "fb.cpool @24");
_Static_assert(offsetof(GuardJSFunctionBytecode, vars) == 32, "fb.vars @32");
_Static_assert(offsetof(GuardJSFunctionBytecode, ext_vars) == 40, "fb.ext_vars @40");
_Static_assert(offsetof(GuardJSFunctionBytecode, stack_size) == 48, "fb.stack_size @48");
_Static_assert(offsetof(GuardJSFunctionBytecode, ext_vars_len) == 50, "fb.ext_vars_len @50");
_Static_assert(offsetof(GuardJSFunctionBytecode, filename) == 56, "fb.filename @56");
_Static_assert(offsetof(GuardJSFunctionBytecode, pc2line) == 64, "fb.pc2line @64");
_Static_assert(offsetof(GuardJSFunctionBytecode, source_pos) == 72, "fb.source_pos @72");
