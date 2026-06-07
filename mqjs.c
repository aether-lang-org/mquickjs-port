/*
 * Micro QuickJS REPL
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
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <fcntl.h>

#include "cutils.h"
#include "readline_tty.h"
#include "mquickjs.h"

uint8_t *load_file(const char *filename, int *plen);
int mqjs_dsl_demo(void);   /* ae/mqjs_dsl: declarative run(){...} demo */
static void dump_error(JSContext *ctx);

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */

JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */

/* host accessors for the Aether CLI builtins (ae/cli_host.ae). */
void *mqjs_stdout(void) { return stdout; }

#if defined(__linux__) || defined(__APPLE__)
static int64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}
#else
static int64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}
#endif

static int64_t get_date_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

/* host time accessors for the Aether CLI builtins (ae/cli_host.ae). */
int64_t mqjs_get_time_ms(void) { return get_time_ms(); }
int64_t mqjs_get_date_ms(void) { return get_date_ms(); }

JSValue js_date_constructor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */

/* load a script */
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv); /* ae/cli_host.ae */

/* timers */
typedef struct {
    BOOL allocated;
    JSGCRef func;
    int64_t timeout; /* in ms */
} JSTimer;

#define MAX_TIMERS 16

static JSTimer js_timer_list[MAX_TIMERS];

static JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSTimer *th;
    int delay, i;
    JSValue *pfunc;
    
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_ToInt32(ctx, &delay, argv[1]))
        return JS_EXCEPTION;
    for(i = 0; i < MAX_TIMERS; i++) {
        th = &js_timer_list[i];
        if (!th->allocated) {
            pfunc = JS_AddGCRef(ctx, &th->func);
            *pfunc = argv[0];
            th->timeout = get_time_ms() + delay;
            th->allocated = TRUE;
            return JS_NewInt32(ctx, i);
        }
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}

static JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int timer_id;
    JSTimer *th;

    if (JS_ToInt32(ctx, &timer_id, argv[0]))
        return JS_EXCEPTION;
    if (timer_id >= 0 && timer_id < MAX_TIMERS) {
        th = &js_timer_list[timer_id];
        if (th->allocated) {
            JS_DeleteGCRef(ctx, &th->func);
            th->allocated = FALSE;
        }
    }
    return JS_UNDEFINED;
}

static void run_timers(JSContext *ctx)
{
    int64_t min_delay, delay, cur_time;
    BOOL has_timer;
    int i;
    JSTimer *th;
    struct timespec ts;

    for(;;) {
        min_delay = 1000;
        cur_time = get_time_ms();
        has_timer = FALSE;
        for(i = 0; i < MAX_TIMERS; i++) {
            th = &js_timer_list[i];
            if (th->allocated) {
                has_timer = TRUE;
                delay = th->timeout - cur_time;
                if (delay <= 0) {
                    JSValue ret;
                    /* the timer expired */
                    if (JS_StackCheck(ctx, 2))
                        goto fail;
                    JS_PushArg(ctx, th->func.val); /* func name */
                    JS_PushArg(ctx, JS_NULL); /* this */
                    
                    JS_DeleteGCRef(ctx, &th->func);
                    th->allocated = FALSE;
                    
                    ret = JS_Call(ctx, 0);
                    if (JS_IsException(ret)) {
                    fail:
                        dump_error(ctx);
                        exit(1);
                    }
                    min_delay = 0;
                    break;
                } else if (delay < min_delay) {
                    min_delay = delay;
                }
            }
        }
        if (!has_timer)
            break;
        if (min_delay > 0) {
            ts.tv_sec = min_delay / 1000;
            ts.tv_nsec = (min_delay % 1000) * 1000000;
            nanosleep(&ts, NULL);
        }
    }
}

#include "mqjs_stdlib.h"

#define STYLE_DEFAULT    COLOR_BRIGHT_GREEN
#define STYLE_COMMENT    COLOR_WHITE
#define STYLE_STRING     COLOR_BRIGHT_CYAN
#define STYLE_REGEX      COLOR_CYAN
#define STYLE_NUMBER     COLOR_GREEN
#define STYLE_KEYWORD    COLOR_BRIGHT_WHITE
#define STYLE_FUNCTION   COLOR_BRIGHT_YELLOW
#define STYLE_TYPE       COLOR_BRIGHT_MAGENTA
#define STYLE_IDENTIFIER COLOR_BRIGHT_GREEN
#define STYLE_ERROR      COLOR_RED
#define STYLE_RESULT     COLOR_BRIGHT_WHITE
#define STYLE_ERROR_MSG  COLOR_BRIGHT_RED

uint8_t *load_file(const char *filename, int *plen)
{
    FILE *f;
    uint8_t *buf;
    int buf_len;

    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    buf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(buf_len + 1);
    fread(buf, 1, buf_len, f);
    buf[buf_len] = '\0';
    fclose(f);
    if (plen)
        *plen = buf_len;
    return buf;
}

static int js_log_err_flag;

static void js_log_func(void *opaque, const void *buf, size_t buf_len)
{
    fwrite(buf, 1, buf_len, js_log_err_flag ? stderr : stdout);
}

static void dump_error(JSContext *ctx)
{
    JSValue obj;
    obj = JS_GetException(ctx);
    fprintf(stderr, "%s", term_colors[STYLE_ERROR_MSG]);
    js_log_err_flag++;
    JS_PrintValueF(ctx, obj, JS_DUMP_LONG);
    js_log_err_flag--;
    fprintf(stderr, "%s\n", term_colors[COLOR_NONE]);
}

static int eval_buf(JSContext *ctx, const char *eval_str, const char *filename, BOOL is_repl, int parse_flags)
{
    JSValue val;
    int flags;
    
    flags = parse_flags;
    if (is_repl)
        flags |= JS_EVAL_RETVAL | JS_EVAL_REPL;
    val = JS_Parse(ctx, eval_str, strlen(eval_str), filename, flags);
    if (JS_IsException(val))
        goto exception;

    val = JS_Run(ctx, val);
    if (JS_IsException(val)) {
    exception:
        dump_error(ctx);
        return 1;
    } else {
        if (is_repl) {
            printf("%s", term_colors[STYLE_RESULT]);
            JS_PrintValueF(ctx, val, JS_DUMP_LONG);
            printf("%s\n", term_colors[COLOR_NONE]);
        }
        return 0;
    }
}

static int eval_file(JSContext *ctx, const char *filename,
                     int argc, const char **argv, int parse_flags,
                     BOOL allow_bytecode)
{
    uint8_t *buf;
    int ret, buf_len;
    JSValue val;
    
    buf = load_file(filename, &buf_len);
    if (allow_bytecode && JS_IsBytecode(buf, buf_len)) {
        if (JS_RelocateBytecode(ctx, buf, buf_len)) {
            fprintf(stderr, "Could not relocate bytecode\n");
            exit(1);
        }
        val = JS_LoadBytecode(ctx, buf);
    } else {
        val = JS_Parse(ctx, (char *)buf, buf_len, filename, parse_flags);
    }
    if (JS_IsException(val))
        goto exception;

    if (argc > 0) {
        JSValue obj, arr;
        JSGCRef arr_ref, val_ref;
        int i;
        
        JS_PUSH_VALUE(ctx, val);
        /* must be defined after JS_LoadBytecode() */
        arr = JS_NewArray(ctx, argc);
        JS_PUSH_VALUE(ctx, arr);
        for(i = 0; i < argc; i++) {
            JS_SetPropertyUint32(ctx, arr_ref.val, i,
                                 JS_NewString(ctx, argv[i]));
        }
        JS_POP_VALUE(ctx, arr);
        obj = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, obj, "scriptArgs", arr);
        JS_POP_VALUE(ctx, val);
    }
    
    
    val = JS_Run(ctx, val);
    if (JS_IsException(val)) {
    exception:
        dump_error(ctx);
        ret = 1;
    } else {
        ret = 0;
    }
    free(buf);
    return ret;
}

static void compile_file(const char *filename, const char *outfilename,
                         size_t mem_size, int dump_memory, int parse_flags, BOOL force_32bit)
{
    uint8_t *mem_buf;
    JSContext *ctx;
    char *eval_str;
    JSValue val;
    union {
        JSBytecodeHeader hdr;
#if JSW == 8
        JSBytecodeHeader32 hdr32;
#endif
    } hdr_buf;
    int hdr_len;
    const uint8_t *data_buf;
    uint32_t data_len;
    FILE *f;
    
    /* When compiling to a file, the actual content of the stdlib does
       not matter because the generated bytecode does not depend on
       it. We still need it so that the atoms for the parsing are
       defined. The JSContext must be discarded once the compilation
       is done. */
    mem_buf = malloc(mem_size);
    ctx = JS_NewContext2(mem_buf, mem_size, &js_stdlib, TRUE);
    JS_SetLogFunc(ctx, js_log_func);

    eval_str = (char *)load_file(filename, NULL);

    val = JS_Parse(ctx, eval_str, strlen(eval_str), filename, parse_flags);
    free(eval_str);
    if (JS_IsException(val)) {
        dump_error(ctx);
        return;
    }

#if JSW == 8 
    if (force_32bit) {
        if (JS_PrepareBytecode64to32(ctx, &hdr_buf.hdr32, &data_buf, &data_len, val)) {
            fprintf(stderr, "Could not convert the bytecode from 64 to 32 bits\n");
            exit(1);
        }
        hdr_len = sizeof(JSBytecodeHeader32);
    } else
#endif
    {
        JS_PrepareBytecode(ctx, &hdr_buf.hdr, &data_buf, &data_len, val);
        
        if (dump_memory)
            JS_DumpMemory(ctx, (dump_memory >= 2));
        
        /* Relocate to zero to have a deterministic
           output. JS_DumpMemory() cannot work once the heap is relocated,
           so we relocate after it. */
        JS_RelocateBytecode2(ctx, &hdr_buf.hdr, (uint8_t *)data_buf, data_len, 0, FALSE);
        hdr_len = sizeof(JSBytecodeHeader);
    }
    f = fopen(outfilename, "wb");
    if (!f) {
        perror(outfilename);
        exit(1);
    }
    fwrite(&hdr_buf, 1, hdr_len, f);
    fwrite(data_buf, 1, data_len, f);
    fclose(f);
    
    JS_FreeContext(ctx);
    free(mem_buf);
}

/* repl */

static ReadlineState readline_state;
static uint8_t readline_cmd_buf[256];
static uint8_t readline_kill_buf[256];
static char readline_history[512];

void readline_find_completion(const char *cmdline)
{
}

BOOL is_word(int c); /* ae/cli_host.ae */

static const char js_keywords[] = 
    "break|case|catch|continue|debugger|default|delete|do|"
    "else|finally|for|function|if|in|instanceof|new|"
    "return|switch|this|throw|try|typeof|while|with|"
    "class|const|enum|import|export|extends|super|"
    "implements|interface|let|package|private|protected|"
    "public|static|yield|"
    "undefined|null|true|false|Infinity|NaN|"
    "eval|arguments|"
    "await|";

static const char js_types[] = "void|var|";

BOOL find_keyword(const char *buf, size_t buf_len, const char *dict); /* ae/cli_host.ae */

/* return the color for the character at position 'pos' and the number
   of characters of the same color */
static int term_get_color(int *plen, const char *buf, int pos, int buf_len)
{
    int c, color, pos1, len;

    c = buf[pos];
    if (c == '"' || c == '\'') {
        pos1 = pos + 1;
        for(;;) {
            if (buf[pos1] == '\0' || buf[pos1] == c)
                break;
            if (buf[pos1] == '\\' && buf[pos1 + 1] != '\0')
                pos1 += 2;
            else
                pos1++;
        }
        if (buf[pos1] != '\0')
            pos1++;
        len = pos1 - pos;
        color = STYLE_STRING;
    } else if (c == '/' && buf[pos + 1] == '*') {
        pos1 = pos + 2;
        while (buf[pos1] != '\0' &&
               !(buf[pos1] == '*' && buf[pos1 + 1] == '/')) {
            pos1++;
        }
        if (buf[pos1] != '\0')
            pos1 += 2;
        len = pos1 - pos;
        color = STYLE_COMMENT;
    } else if ((c >= '0' && c <= '9') || c == '.') {
        pos1 = pos + 1;
        while (is_word(buf[pos1]))
            pos1++;
        len = pos1 - pos;
        color = STYLE_NUMBER;
    } else if (is_word(c)) {
        pos1 = pos + 1;
        while (is_word(buf[pos1]))
            pos1++;
        len = pos1 - pos;
        if (find_keyword(buf + pos, len, js_keywords)) {
            color = STYLE_KEYWORD;
        } else {
            while (buf[pos1] == ' ')
                pos1++;
            if (buf[pos1] == '(') {
                color = STYLE_FUNCTION;
            } else {
                if (find_keyword(buf + pos, len, js_types)) {
                    color = STYLE_TYPE;
                } else {
                    color = STYLE_IDENTIFIER;
                }
            }
        }
    } else {
        color = STYLE_DEFAULT;
        len = 1;
    }
    *plen = len;
    return color;
}

static int js_interrupt_handler(JSContext *ctx, void *opaque)
{
    return readline_is_interrupted();
}

static void repl_run(JSContext *ctx)
{
    ReadlineState *s = &readline_state;
    const char *cmd;

    s->term_width = readline_tty_init();
    s->term_cmd_buf = readline_cmd_buf;
    s->term_kill_buf = readline_kill_buf;
    s->term_cmd_buf_size = sizeof(readline_cmd_buf);
    s->term_history = readline_history;
    s->term_history_buf_size = sizeof(readline_history);
    s->get_color = term_get_color;

    JS_SetInterruptHandler(ctx, js_interrupt_handler);

    for(;;) {
        cmd = readline_tty(&readline_state, "mqjs > ", FALSE);
        if (!cmd)
            break;
        eval_buf(ctx, cmd, "<cmdline>", TRUE, 0);
        run_timers(ctx);
    }
}

/* ---- Declarative launch DSL (ae/mqjs_dsl) host backing ----------------
 *
 * The Aether builder grammar in ae/mqjs_dsl populates this process-global
 * RunSpec by calling the aether_mqjs_* setters, then aether_mqjs_launch()
 * drains it here — reusing this file's eval_file/eval_buf/compile_file/
 * repl_run/run_timers statics. Steps run in source order; config applies
 * to the run as a whole. */

#define MQJS_DSL_MAX_STEPS 64
#define MQJS_DSL_MAX_ARGS  32

enum {
    MQJS_STEP_INCLUDE, MQJS_STEP_EVAL, MQJS_STEP_SCRIPT,
    MQJS_STEP_COMPILE, MQJS_STEP_INTERACTIVE,
};

typedef struct {
    int kind;
    const char *source;       /* include/eval text or script/compile-input path */
    const char *out_path;     /* compile output */
    int force_32bit;          /* compile: 32-bit bytecode */
    const char *args[MQJS_DSL_MAX_ARGS];
    int args_n;
} MqjsStep;

typedef struct {
    size_t mem_size;
    int parse_flags;
    int dump_memory;
    int allow_bytecode;
    MqjsStep steps[MQJS_DSL_MAX_STEPS];
    int steps_n;
} MqjsRunSpec;

static MqjsRunSpec mqjs_run_spec;

void aether_mqjs_spec_reset(void)
{
    memset(&mqjs_run_spec, 0, sizeof(mqjs_run_spec));
    mqjs_run_spec.mem_size = 16 << 20;
}
void *aether_mqjs_spec_get(void) { return &mqjs_run_spec; }

/* Builder config factory: reset the spec and return its address (pushed
 * as the builder context before the run(){...} block executes). */
void *aether_mqjs_spec_new(void)
{
    aether_mqjs_spec_reset();
    return &mqjs_run_spec;
}

/* Run-wide config. */
void aether_mqjs_memory_limit_mb(void *ctx, int mb)
{ (void)ctx; mqjs_run_spec.mem_size = (size_t)mb << 20; }
void aether_mqjs_no_column(void *ctx)
{ (void)ctx; mqjs_run_spec.parse_flags |= JS_EVAL_STRIP_COL; }
void aether_mqjs_dump_memory(void *ctx, int level)
{ (void)ctx; mqjs_run_spec.dump_memory = level; }
void aether_mqjs_allow_bytecode(void *ctx)
{ (void)ctx; mqjs_run_spec.allow_bytecode = TRUE; }

/* Steps (appended to the queue in source order). */
static MqjsStep *mqjs_dsl_new_step(int kind)
{
    if (mqjs_run_spec.steps_n >= MQJS_DSL_MAX_STEPS) {
        fprintf(stderr, "mqjs DSL: too many steps\n");
        exit(1);
    }
    MqjsStep *s = &mqjs_run_spec.steps[mqjs_run_spec.steps_n++];
    s->kind = kind;
    return s;
}
void aether_mqjs_include(void *ctx, const char *path)
{ (void)ctx; mqjs_dsl_new_step(MQJS_STEP_INCLUDE)->source = path; }
void aether_mqjs_eval(void *ctx, const char *source)
{ (void)ctx; mqjs_dsl_new_step(MQJS_STEP_EVAL)->source = source; }
void aether_mqjs_script(void *ctx, const char *path)
{ (void)ctx; mqjs_dsl_new_step(MQJS_STEP_SCRIPT)->source = path; }
void aether_mqjs_compile(void *ctx)
{ (void)ctx; mqjs_dsl_new_step(MQJS_STEP_COMPILE); }
void aether_mqjs_interactive(void *ctx)
{ (void)ctx; mqjs_dsl_new_step(MQJS_STEP_INTERACTIVE); }

/* Nested setters — operate on the most recently appended step. */
static MqjsStep *mqjs_dsl_cur_step(void)
{
    if (mqjs_run_spec.steps_n == 0) {
        fprintf(stderr, "mqjs DSL: nested setter outside a step\n");
        exit(1);
    }
    return &mqjs_run_spec.steps[mqjs_run_spec.steps_n - 1];
}
void aether_mqjs_arg(void *ctx, const char *value)
{
    (void)ctx;
    MqjsStep *s = mqjs_dsl_cur_step();
    if (s->args_n < MQJS_DSL_MAX_ARGS)
        s->args[s->args_n++] = value;
}
void aether_mqjs_input(void *ctx, const char *path)
{ (void)ctx; mqjs_dsl_cur_step()->source = path; }
void aether_mqjs_output(void *ctx, const char *path)
{ (void)ctx; mqjs_dsl_cur_step()->out_path = path; }
void aether_mqjs_force_32bit(void *ctx)
{ (void)ctx; mqjs_dsl_cur_step()->force_32bit = TRUE; }

/* Drain the spec: create one engine, run every step in order, tear down.
 * Returns 0 on success, 1 if any step failed. Compile steps run in their
 * own context (compile_file owns its engine), matching the CLI. */
int aether_mqjs_launch(void)
{
    MqjsRunSpec *spec = &mqjs_run_spec;
    uint8_t *mem_buf;
    JSContext *ctx;
    int i, rc = 0, want_interactive = 0;

    /* Compile steps are standalone (own engine); handle them first/inline. */
    for (i = 0; i < spec->steps_n; i++) {
        MqjsStep *s = &spec->steps[i];
        if (s->kind == MQJS_STEP_COMPILE) {
            compile_file(s->source, s->out_path, spec->mem_size,
                         spec->dump_memory, spec->parse_flags, s->force_32bit);
        }
    }
    /* If every step was a compile, we're done. */
    {
        int only_compiles = 1;
        for (i = 0; i < spec->steps_n; i++)
            if (spec->steps[i].kind != MQJS_STEP_COMPILE) only_compiles = 0;
        if (spec->steps_n > 0 && only_compiles)
            return 0;
    }

    mem_buf = malloc(spec->mem_size);
    ctx = JS_NewContext(mem_buf, spec->mem_size, &js_stdlib);
    JS_SetLogFunc(ctx, js_log_func);
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        JS_SetRandomSeed(ctx, ((uint64_t)tv.tv_sec << 32) ^ tv.tv_usec);
    }

    for (i = 0; i < spec->steps_n && rc == 0; i++) {
        MqjsStep *s = &spec->steps[i];
        switch (s->kind) {
        case MQJS_STEP_INCLUDE:
            if (eval_file(ctx, s->source, 0, NULL, spec->parse_flags,
                          spec->allow_bytecode)) rc = 1;
            break;
        case MQJS_STEP_EVAL:
            if (eval_buf(ctx, s->source, "<cmdline>", FALSE,
                         spec->parse_flags | JS_EVAL_REPL)) rc = 1;
            break;
        case MQJS_STEP_SCRIPT:
            if (eval_file(ctx, s->source, s->args_n, s->args,
                          spec->parse_flags, spec->allow_bytecode)) rc = 1;
            break;
        case MQJS_STEP_INTERACTIVE:
            want_interactive = 1;
            break;
        case MQJS_STEP_COMPILE:
            break; /* already done */
        }
    }

    if (rc == 0) {
        if (want_interactive) repl_run(ctx);
        else run_timers(ctx);
        if (spec->dump_memory)
            JS_DumpMemory(ctx, (spec->dump_memory >= 2));
    }

    JS_FreeContext(ctx);
    free(mem_buf);
    return rc;
}

static void help(void)
{
    printf("MicroQuickJS" "\n"
           "usage: mqjs [options] [file [args]]\n"
           "-h  --help            list options\n"
           "-e  --eval EXPR       evaluate EXPR\n"
           "-i  --interactive     go to interactive mode\n"
           "-I  --include file    include an additional file\n"
           "-d  --dump            dump the memory usage stats\n"
           "    --memory-limit n  limit the memory usage to 'n' bytes\n"
           "--no-column           no column number in debug information\n"
           "-o FILE               save the bytecode to FILE\n"
           "-m32                  force 32 bit bytecode output (use with -o)\n"
           "-b  --allow-bytecode  allow bytecode in input file\n");
    exit(1);
}

int main(int argc, const char **argv)
{
    int optind;
    size_t mem_size;
    int dump_memory = 0;
    int interactive = 0;
    const char *expr = NULL;
    const char *out_filename = NULL;
    const char *include_list[32];
    int include_count = 0;
    uint8_t *mem_buf;
    JSContext *ctx;
    int i, parse_flags;
    BOOL force_32bit, allow_bytecode;
    
    mem_size = 16 << 20;
    dump_memory = 0;
    parse_flags = 0;
    force_32bit = FALSE;
    allow_bytecode = FALSE;

    /* --dsl-demo: exercise the declarative ae/mqjs_dsl run(){...} builder
       against the real engine (used by the conformance gate). */
    if (argc >= 2 && !strcmp(argv[1], "--dsl-demo"))
        return mqjs_dsl_demo();

    /* cannot use getopt because we want to pass the command line to
       the script */
    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        const char *arg = argv[optind] + 1;
        const char *longopt = "";
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help();
                continue;
            }
            if (opt == 'e' || !strcmp(longopt, "eval")) {
                if (*arg) {
                    expr = arg;
                    break;
                }
                if (optind < argc) {
                    expr = argv[optind++];
                    break;
                }
                fprintf(stderr, "missing expression for -e\n");
                exit(2);
            }
            if (!strcmp(longopt, "memory-limit")) {
                char *p;
                double count;
                if (optind >= argc) {
                    fprintf(stderr, "expecting memory limit");
                    exit(1);
                }
                count = strtod(argv[optind++], &p);
                switch (tolower((unsigned char)*p)) {
                case 'g':
                    count *= 1024;
                    /* fall thru */
                case 'm':
                    count *= 1024;
                    /* fall thru */
                case 'k':
                    count *= 1024;
                    /* fall thru */
                default:
                    mem_size = (size_t)(count);
                    break;
                }
                continue;
            }
            if (opt == 'd' || !strcmp(longopt, "dump")) {
                dump_memory++;
                continue;
            }
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                continue;
            }
            if (opt == 'o') {
                if (*arg) {
                    out_filename = arg;
                    break;
                }
                if (optind < argc) {
                    out_filename = argv[optind++];
                    break;
                }
                fprintf(stderr, "missing filename for -o\n");
                exit(2);
            }
            if (opt == 'I' || !strcmp(longopt, "include")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting filename");
                    exit(1);
                }
                if (include_count >= countof(include_list)) {
                    fprintf(stderr, "too many included files");
                    exit(1);
                }
                include_list[include_count++] = argv[optind++];
                continue;
            }
            if (!strcmp(longopt, "no-column")) {
                parse_flags |= JS_EVAL_STRIP_COL;
                continue;
            }
            if (opt == 'm' && !strcmp(arg, "32")) {
                /* XXX: using a long option is not consistent here */
                force_32bit = TRUE;
                arg += strlen(arg);
                continue;
            }
            if (opt == 'b' || !strcmp(longopt, "allow-bytecode")) {
                allow_bytecode = TRUE;
                continue;
            }
            if (opt) {
                fprintf(stderr, "qjs: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "qjs: unknown option '--%s'\n", longopt);
            }
            help();
        }
    }

    if (out_filename) {
        if (optind >= argc) {
            fprintf(stderr, "expecting input filename\n");
            exit(1);
        }
        compile_file(argv[optind], out_filename, mem_size, dump_memory,
                     parse_flags, force_32bit);
    } else {
        mem_buf = malloc(mem_size);
        ctx = JS_NewContext(mem_buf, mem_size, &js_stdlib);
        JS_SetLogFunc(ctx, js_log_func);
        {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            JS_SetRandomSeed(ctx, ((uint64_t)tv.tv_sec << 32) ^ tv.tv_usec);
        }

        for(i = 0; i < include_count; i++) {
            if (eval_file(ctx, include_list[i], 0, NULL,
                          parse_flags, allow_bytecode)) {
                goto fail;
            }
        }
        
        if (expr) {
            if (eval_buf(ctx, expr, "<cmdline>", FALSE, parse_flags | JS_EVAL_REPL))
                goto fail;
        } else if (optind >= argc) {
            interactive = 1;
        } else {
            if (eval_file(ctx, argv[optind], argc - optind, argv + optind,
                          parse_flags, allow_bytecode)) {
                goto fail;
            }
        }
        
        if (interactive) {
            repl_run(ctx);
        } else {
            run_timers(ctx);
        }
        
        if (dump_memory)
            JS_DumpMemory(ctx, (dump_memory >= 2));
        
        JS_FreeContext(ctx);
        free(mem_buf);
    }
    return 0;
 fail:
    JS_FreeContext(ctx);
    free(mem_buf);
    return 1;
}
