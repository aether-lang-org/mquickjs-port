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
                        JSPropsKindEnum props_kind, const char *class_id_str)
{
    int i, *ident_tab, idx, props_ident, n_props;
    int prop_idx;
    const JSPropDef *d;
    uint32_t *prop_hash;
    BOOL is_global_object = (props_kind == PROPS_KIND_GLOBAL);
    static const JSPropDef dummy_props[] = {
        { JS_DEF_END },
    };

    if (!props_def)
        props_def = dummy_props;
    
    n_props = 0;
    for(d = props_def; d->def_type != JS_DEF_END; d++) {
        n_props++;
    }
    if (props_kind == PROPS_KIND_PROTO ||
        props_kind == PROPS_KIND_CLASS)
        n_props++;
    ident_tab = malloc(sizeof(ident_tab[0]) * n_props);

    /* define the various objects */
    for(d = props_def, i = 0; d->def_type != JS_DEF_END; d++, i++) {
        ident_tab[i] = define_value(s, d);
    }

    props_ident = -1;
    prop_hash = NULL;
    if (is_global_object) {
        props_ident = s->cur_offset;
        printf("  /* global object properties (offset=%d) */\n", props_ident);
        printf("  JS_VALUE_ARRAY_HEADER(%d),\n", 2 * n_props);
        s->cur_offset += 2 * n_props + 1;
    } else {
        int hash_size_log2;
        uint32_t hash_size, hash_mask;
        uint32_t *hash_table, h;
        
        if (n_props <= 1)
            hash_size_log2 = 0;
        else
            hash_size_log2 = (32 - clz32(n_props - 1)) - 1;
        hash_size = 1 << hash_size_log2;
        if (hash_size > ATOM_ALIGN / JSW) {
#if !defined __APPLE__
            // XXX: Cannot request data alignment larger than 64 bytes on Darwin
            fprintf(stderr, "Too many properties, consider increasing ATOM_ALIGN\n");
#endif
            hash_size = ATOM_ALIGN / JSW;
        }
        hash_mask = hash_size - 1;

        hash_table = malloc(sizeof(hash_table[0]) * hash_size);
        prop_hash = malloc(sizeof(prop_hash[0]) * n_props);
        /* build the hash table */
        for(i = 0; i < hash_size; i++)
            hash_table[i] = 0;
        prop_idx = 0;
        for(i = 0, d = props_def; i < n_props; i++, d++) {
            const char *name;
            if (d->def_type != JS_DEF_END) {
                name = d->name;
            } else {
                if (props_kind == PROPS_KIND_PROTO)
                    name = "constructor";
                else
                    name = "prototype";
            }
            h = hash_prop(s, name) & hash_mask;
            prop_hash[prop_idx] = hash_table[h];
            hash_table[h] = 2 + hash_size + 3 * prop_idx;
            prop_idx++;
        }

        props_ident = s->cur_offset;
        printf("  /* properties (offset=%d) */\n", props_ident);
        printf("  JS_VALUE_ARRAY_HEADER(%d),\n", 2 + hash_size + n_props * 3);
        printf("  %d << 1, /* n_props */\n", n_props);
        printf("  %d << 1, /* hash_mask */\n", hash_mask);
        for(i = 0; i < hash_size; i++) {
            printf("  %d << 1,\n", hash_table[i]);
        }
        s->cur_offset += hash_size + 3 + 3 * n_props;
        free(hash_table);
    }
    prop_idx = 0;
    for(d = props_def, i = 0; i < n_props; d++, i++) {
        const char *name, *prop_type;
        /* name */
        printf("  ");
        if (d->def_type != JS_DEF_END) {
            name = d->name;
        } else {
            if (props_kind == PROPS_KIND_PROTO)
                name = "constructor";
            else
                name = "prototype";
        }
        dump_atom(s, name, FALSE);
        printf(",\n");

        printf("  ");
        prop_type = "NORMAL";
        switch(d->def_type) {
        case JS_DEF_PROP_DOUBLE:
            if (ident_tab[i] >= 0)
                goto value_ptr;
            /* short int */
            printf("%d << 1,", (int32_t)d->u.f64);
            break;
        case JS_DEF_CGETSET:
            if (is_global_object) {
                fprintf(stderr, "getter/setter forbidden in global object\n");
                exit(1);
            }
            prop_type = "GETSET";
            goto value_ptr;
        case JS_DEF_CLASS:
            if (!is_global_object) {
                fprintf(stderr, "class definition only allowed in global object\n");
                exit(1);
            }
        value_ptr:
            assert(ident_tab[i] >= 0);
            printf("JS_ROM_VALUE(%d),", ident_tab[i]);
            break;
        case JS_DEF_PROP_UNDEFINED:
            printf("JS_UNDEFINED,");
            break;
        case JS_DEF_PROP_NULL:
            printf("JS_NULL,");
            break;
        case JS_DEF_PROP_STRING:
            dump_atom(s, d->u.str, FALSE);
            printf(",");
            break;
        case JS_DEF_CFUNC:
            idx = add_cfunc(&s->cfunc_list,
                            d->name,
                            d->u.func.length,
                            d->u.func.magic,
                            d->u.func.cproto_name,
                            d->u.func.func_name);
            printf("JS_VALUE_MAKE_SPECIAL(JS_TAG_SHORT_FUNC, %d),", idx);
            break;
        case JS_DEF_END:
            if (props_kind == PROPS_KIND_PROTO) {
                /* constructor property */
                printf("(uint32_t)(-%s - 1) << 1,", class_id_str);
            } else {
                /* prototype property */
                printf("%s << 1,", class_id_str);
            }
            prop_type = "SPECIAL";
            break;
        default:
            abort();
        }
        printf("\n");
        if (!is_global_object) {
            printf("  (%d << 1) | (JS_PROP_%s << 30),\n",
                   prop_hash[prop_idx], prop_type);
        }
        prop_idx++;
    }

    free(prop_hash);
    free(ident_tab);
    return props_ident;
}

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
