/*
 * Micro QuickJS build utility
 *
 * Copyright (c) 2017-2025 Fabrice Bellard
 * Copyright (c) 2017-2025 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "cutils.h"
#include "list.h"
#include "mquickjs_build.h"

static unsigned JSW = 4; // override this with -m64

/* accessor so the Aether-ported build-tool helpers can read the host
   word size selected by the -m option. */
int bt_get_jsw(void) { return JSW; }
void *stderr_ptr(void) { return stderr; }

typedef struct {
    char *str;
    int offset;
} AtomDef;

typedef struct {
    AtomDef *tab;
    int count;
    int size;
    int offset;
} AtomList;

typedef struct {
    char *name;
    int length;
    char *magic;
    char *cproto_name;
    char *cfunc_name;
} CFuncDef;

typedef struct {
    CFuncDef *tab;
    int count;
    int size;
} CFuncList;

typedef struct {
    struct list_head link;
    const JSClassDef *class1;
    int class_idx;
    char *finalizer_name;
    char *class_id;
} ClassDefEntry;

typedef struct {
    AtomList atom_list;
    CFuncList cfunc_list;
    int cur_offset;
    int sorted_atom_table_offset;
    int global_object_offset;
    struct list_head class_list;
} BuildContext;

static const char *atoms[] = {
#define DEF(a, b) b,
    /* keywords */
    DEF(null, "null") /* must be first */
    DEF(false, "false")
    DEF(true, "true")
    DEF(if, "if")
    DEF(else, "else")
    DEF(return, "return")
    DEF(var, "var")
    DEF(this, "this")
    DEF(delete, "delete")
    DEF(void, "void")
    DEF(typeof, "typeof")
    DEF(new, "new")
    DEF(in, "in")
    DEF(instanceof, "instanceof")
    DEF(do, "do")
    DEF(while, "while")
    DEF(for, "for")
    DEF(break, "break")
    DEF(continue, "continue")
    DEF(switch, "switch")
    DEF(case, "case")
    DEF(default, "default")
    DEF(throw, "throw")
    DEF(try, "try")
    DEF(catch, "catch")
    DEF(finally, "finally")
    DEF(function, "function")
    DEF(debugger, "debugger")
    DEF(with, "with")
    /* FutureReservedWord */
    DEF(class, "class")
    DEF(const, "const")
    DEF(enum, "enum")
    DEF(export, "export")
    DEF(extends, "extends")
    DEF(import, "import")
    DEF(super, "super")
    /* FutureReservedWords when parsing strict mode code */
    DEF(implements, "implements")
    DEF(interface, "interface")
    DEF(let, "let")
    DEF(package, "package")
    DEF(private, "private")
    DEF(protected, "protected")
    DEF(public, "public")
    DEF(static, "static")
    DEF(yield, "yield")
#undef DEF

    /* other atoms */
    "",
    "toString",
    "valueOf",
    "number",
    "object",
    "undefined",
    "string",
    "boolean",
    "<ret>",
    "<eval>",
    "eval",
    "arguments",
    "value",
    "get",
    "set",
    "prototype",
    "constructor",
    "length",
    "target",
    "of",
    "NaN",
    "Infinity",
    "-Infinity",
    "name",
    "Error",
    "__proto__",
    "index",
    "input",
};


static char *cvt_name(char *buf, size_t buf_size, const char *str)
{
    size_t i, len = strlen(str);
    assert(len < buf_size);
    if (len == 0) {
        strcpy(buf, "empty");
    } else {
        strcpy(buf, str);
        for(i = 0; i < len; i++) {
            if (buf[i] == '<' || buf[i] == '>' || buf[i] == '-')
                buf[i] = '_';
        }
    }
    return buf;
}

BOOL is_ascii_string(const char *buf, size_t len); /* gen/buildtool/bt_predicates.ae */

BOOL is_numeric_string(const char *buf, size_t len); /* gen/buildtool/bt_predicates.ae */

int find_atom(AtomList *s, const char *str); /* gen/buildtool/bt_atomlist.ae */

int add_atom(AtomList *s, const char *str); /* gen/buildtool/bt_atomlist.ae */

int add_cfunc(CFuncList *s, const char *name, int length, const char *magic, const char *cproto_name, const char *cfunc_name); /* gen/buildtool/bt_atomlist.ae */

void dump_atom_defines(void); /* gen/genengine/module.ae */

int atom_cmp(const void *p1, const void *p2); /* gen/buildtool/bt_atomlist.ae */

/* js_atom_table must be properly aligned because the property hash
   table uses the low bits of the atom pointer value */
#define ATOM_ALIGN 64

void dump_atoms(BuildContext *ctx); /* gen/genengine/module.ae */


/* dump_atom / dump_cfuncs live in gen/genengine/module.ae (Aether). The C
   callers below use dump_atom via this thin alias to ge_dump_atom. */
uint32_t ge_dump_atom(BuildContext *s, const char *str, int value_only); /* genengine */
static uint32_t dump_atom(BuildContext *s, const char *str, BOOL value_only)
{
    return ge_dump_atom(s, str, value_only);
}

void dump_cfuncs(BuildContext *s); /* gen/genengine/module.ae */


void dump_cfinalizers(BuildContext *s); /* gen/genengine/module.ae */

typedef enum {
    PROPS_KIND_GLOBAL,
    PROPS_KIND_PROTO,
    PROPS_KIND_CLASS,
    PROPS_KIND_OBJECT,
} JSPropsKindEnum;

static inline uint32_t hash_prop(BuildContext *s, const char *name)
{
    /* Compute the hash for a symbol, must be consistent with
       mquickjs.c implementation.
     */
    uint32_t prop = dump_atom(s, name, TRUE);
    return (prop / JSW) ^ (prop % JSW); /* XXX: improve */
}

int define_props(BuildContext *s, const JSPropDef *props_def,
                 JSPropsKindEnum props_kind, const char *class_id_str); /* gen/genengine/module.ae */

static ClassDefEntry *find_class(BuildContext *s, const JSClassDef *d)
{
    struct list_head *el;
    ClassDefEntry *e;
    
    list_for_each(el, &s->class_list) {
        e = list_entry(el, ClassDefEntry, link);
        if (e->class1 == d)
            return e;
    }
    return NULL;
}

static void free_class_entries(BuildContext *s)
{
    struct list_head *el, *el1;
    ClassDefEntry *e;
    list_for_each_safe(el, el1, &s->class_list) {
        e = list_entry(el, ClassDefEntry, link);
        free(e->class_id);
        free(e->finalizer_name);
        free(e);
    }
    init_list_head(&s->class_list);
}

int define_class(BuildContext *s, const JSClassDef *d); /* gen/genengine/module.ae */

#define JS_SHORTINT_MIN (-(1 << 30))
#define JS_SHORTINT_MAX ((1 << 30) - 1)

BOOL is_short_int(double d); /* gen/buildtool/bt_predicates.ae */

int define_value(BuildContext *s, const JSPropDef *d); /* gen/genengine/module.ae */

/* define_atoms_props / define_atoms_class live in gen/genengine/module.ae */
void define_atoms_props(BuildContext *s, const JSPropDef *props_def, JSPropsKindEnum props_kind);
void define_atoms_class(BuildContext *s, const JSClassDef *d);

static int usage(const char *name)
{
    fprintf(stderr, "usage: %s {-m32 | -m64} [-a]\n", name);
    fprintf(stderr,
            "    create a ROM file for the mquickjs standard library\n"
            "--help       list options\n"
            "-m32         force generation for a 32 bit target\n"
            "-m64         force generation for a 64 bit target\n"
            "-a           generate the mquickjs_atom.h header\n"
            );
    return 1;
}

int build_atoms(const char *stdlib_name, const JSPropDef *global_obj,
                const JSPropDef *c_function_decl, int argc, char **argv)
{
    int i;
    unsigned jsw;
    BuildContext ss, *s = &ss;
    BOOL build_atom_defines = FALSE;
    
#if INTPTR_MAX >= INT64_MAX
    jsw = 8;
#else
    jsw = 4;
#endif    
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m64")) {
            jsw = 8;
        } else if (!strcmp(argv[i], "-m32")) {
            jsw = 4;
        } else if (!strcmp(argv[i], "-a")) {
            build_atom_defines = TRUE;
        } else if (!strcmp(argv[i], "--help")) {
            return usage(argv[0]);
        } else {
            fprintf(stderr, "invalid argument '%s'\n", argv[i]);
            return usage(argv[0]);
        }
    }

    JSW = jsw;
    
    if (build_atom_defines) {
        dump_atom_defines();
        return 0;
    }
    
    memset(s, 0, sizeof(*s));
    init_list_head(&s->class_list);

    /* add the predefined atoms (they have a corresponding define) */
    for(i = 0; i < countof(atoms); i++) {
        add_atom(&s->atom_list, atoms[i]);
    }

    /* add the predefined functions */
    if (c_function_decl) {
        const JSPropDef *d;
        for(d = c_function_decl; d->def_type != JS_DEF_END; d++) {
            if (d->def_type != JS_DEF_CFUNC) {
                fprintf(stderr, "only C functions are allowed in c_function_decl[]\n");
                exit(1);
            }
            add_atom(&s->atom_list, d->name);
            add_cfunc(&s->cfunc_list,
                      d->name,
                      d->u.func.length,
                      d->u.func.magic,
                      d->u.func.cproto_name,
                      d->u.func.func_name);
        }
    }

    /* first pass to define the atoms */
    define_atoms_props(s, global_obj, PROPS_KIND_GLOBAL);
    free_class_entries(s);

    printf("/* this file is automatically generated - do not edit */\n\n");
    printf("#include \"mquickjs_priv.h\"\n\n");
    
    printf("static const uint%u_t __attribute((aligned(%d))) js_stdlib_table[] = {\n",
           JSW * 8, ATOM_ALIGN);

    dump_atoms(s);

    s->global_object_offset = define_props(s, global_obj, PROPS_KIND_GLOBAL, NULL);

    printf("};\n\n");

    dump_cfuncs(s);
    
    printf("#ifndef JS_CLASS_COUNT\n"
           "#define JS_CLASS_COUNT JS_CLASS_USER /* total number of classes */\n"
           "#endif\n\n");

    dump_cfinalizers(s);

    free_class_entries(s);

    printf("const JSSTDLibraryDef %s = {\n", stdlib_name);
    printf("  js_stdlib_table,\n");
    printf("  js_c_function_table,\n");
    printf("  js_c_finalizer_table,\n");
    printf("  %d,\n", s->cur_offset);
    printf("  %d,\n", ATOM_ALIGN);
    printf("  %d,\n", s->sorted_atom_table_offset);
    printf("  %d,\n", s->global_object_offset);
    printf("  JS_CLASS_COUNT,\n");
    printf("};\n\n");

    return 0;
}
