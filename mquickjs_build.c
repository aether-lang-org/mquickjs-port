/*
 * Micro QuickJS build utility — host-state glue.
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

/* All of the stdlib/atom generator logic now lives in Aether:
 *   gen/genengine/module.ae   — the walker/emitter + the build_atoms entry
 *   gen/buildtool/bt_*.ae      — atom/cfunc list helpers + predicates
 *
 * What must stay C is the host word size selected by the -m32/-m64 option
 * (a mutable global the Aether side reads/writes through accessors) and a
 * stderr handle for the Aether error paths. */

#include <stdio.h>

static unsigned JSW = 4; /* set by build_atoms via bt_set_jsw */

int bt_get_jsw(void) { return JSW; }
void bt_set_jsw(int w) { JSW = w; }
void *stderr_ptr(void) { return stderr; }
