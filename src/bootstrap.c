/*
 * QuickJS libuv bindings
 *
 * Copyright (c) 2019-present Saúl Ibarra Corretgé <s@saghul.net>
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

#include "embedjs.h"
#include "private.h"
#include "tjs.h"

INCTXT(bundle, BOOTSTRAP_BUNDLE_FILENAME);
/**
 * These are defined now:
 *
 * const unsigned char tjs__code_bundle_data[];
 * const unsigned char *const tjs__code_bundle_end;
 * const unsigned int tjs__code_bundle_size;
 *
 */

INCTXT(std, BOOTSTRAP_STD_FILENAME);

/**
 * These are defined now:
 *
 * const unsigned char tjs__code_std_data[];
 * const unsigned char *const tjs__code_std_end;
 * const unsigned int tjs__code_std_size;
 *
 */


int tjs__eval_text(JSContext *ctx, const char *buf, size_t buf_len, const char *filename) {
    int ret = 0;
    JSValue val = JS_Eval(ctx, buf, buf_len - 1, filename, JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        tjs_dump_error(ctx);
        ret = -1;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

int tjs__eval_bytecode(JSContext *ctx, const char *buf, size_t buf_len, const char *filename) {
    int ret = 0;
    JSValue func_val = JS_UNDEFINED;
    JSValue val = JS_UNDEFINED;

    /* FIXME: Work out where this goes and is cleaned up? Global scope? */
    func_val = JS_ReadObject(ctx, (const uint8_t *)buf, buf_len - 1, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(func_val)) {
        tjs_dump_error(ctx);
        ret = -1;
        goto error;
    }

    val = JS_EvalFunction(ctx, func_val);
    if (JS_IsException(val)) {
        tjs_dump_error(ctx);
        ret = -1;
        goto error;
    }

error:
    JS_FreeValue(ctx, val);
    return ret;
}

void tjs__bootstrap_globals(JSContext *ctx) {
#ifdef BOOTSTRAP_BYTECODE
    CHECK_EQ(0, tjs__eval_bytecode(ctx, tjs__code_bundle_data, tjs__code_bundle_size, BOOTSTRAP_BUNDLE_FILENAME));
#else
    CHECK_EQ(0, tjs__eval_text(ctx, tjs__code_bundle_data, tjs__code_bundle_size, BOOTSTRAP_BUNDLE_FILENAME));
#endif
}

static int tjs__add_builtin_module(JSContext *ctx, const char *name, const char *buf, size_t buf_len) {
    JSValue obj = JS_Eval(ctx, buf, buf_len - 1, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj))
        goto error;

    js_module_set_import_meta(ctx, obj, FALSE, FALSE);

    JSValue val = JS_EvalFunction(ctx, obj);
    if (JS_IsException(val))
        goto error;
    JS_FreeValue(ctx, val);

    return 0;

error:
    tjs_dump_error(ctx);
    return -1;
}

static int tjs__add_builtin_module_bytecode(JSContext *ctx, const char *name, const char *buf, size_t buf_len) {
    JSValue obj = JS_ReadObject(ctx, (const uint8_t *)buf, buf_len - 1, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj))
        goto error;

    if (JS_ResolveModule(ctx, obj) < 0)
        goto error;

    js_module_set_import_meta(ctx, obj, FALSE, FALSE);
    return 0;

error:
    tjs_dump_error(ctx);
    return -1;
}

void tjs__add_stdlib(JSContext *ctx) {
#ifdef BOOTSTRAP_BYTECODE
    CHECK_EQ(0, tjs__add_builtin_module_bytecode(ctx, "@tjs/std", tjs__code_std_data, tjs__code_std_size));
#else
    CHECK_EQ(0, tjs__add_builtin_module(ctx, "@tjs/std", tjs__code_std_data, tjs__code_std_size));
#endif
}
