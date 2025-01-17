/*
 * txiki.js
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

#include "curl-utils.h"
#include "private.h"
#include "tjs.h"
#include "utils.h"

#include <string.h>
#include <libgen.h>


JSModuleDef *tjs__load_http(JSContext *ctx, const char *url) {
    JSModuleDef *m;
    DynBuf dbuf;

    dbuf_init(&dbuf);

    int r = tjs_curl_load_http(&dbuf, url);
    if (r != 200) {
        m = NULL;
        JS_ThrowReferenceError(ctx, "could not load '%s' code: %d", url, r);
        goto end;
    }

    /* compile the module */
    JSValue func_val = JS_Eval(ctx, (char *) dbuf.buf, dbuf.size - 1, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        m = NULL;
        goto end;
    }

    /* XXX: could propagate the exception */
    js_module_set_import_meta(ctx, func_val, FALSE, FALSE);
    /* the module is already referenced, so we must free it */
    m = JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);

end:
    /* free the memory we allocated */
    dbuf_free(&dbuf);

    return m;
}

JSModuleDef *tjs_module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    static const char http[] = "http://";
    static const char https[] = "https://";
    static const char json_tpl_start[] = "export default JSON.parse(`";
    static const char json_tpl_end[] = "`);";

    JSModuleDef *m;
    JSValue func_val;
    int r, is_json;
    DynBuf dbuf;

    if (strncmp(http, module_name, strlen(http)) == 0 || strncmp(https, module_name, strlen(https)) == 0) {
        return tjs__load_http(ctx, module_name);
    }

    dbuf_init(&dbuf);

    is_json = has_suffix(module_name, ".json");

    /* Support importing JSON files bcause... why not? */
    if (is_json)
        dbuf_put(&dbuf, (const uint8_t *) json_tpl_start, strlen(json_tpl_start));

    r = tjs__load_file(ctx, &dbuf, module_name);
    if (r != 0) {
        dbuf_free(&dbuf);
        JS_ThrowReferenceError(ctx, "could not load '%s'", module_name);
        return NULL;
    }

    if (is_json)
        dbuf_put(&dbuf, (const uint8_t *) json_tpl_end, strlen(json_tpl_end));

    /* Add null termination, required by JS_Eval. */
    dbuf_putc(&dbuf, '\0');

    /* compile JS the module */
    func_val = JS_Eval(ctx, (char *) dbuf.buf, dbuf.size - 1, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    dbuf_free(&dbuf);
    if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        return NULL;
    }

    /* XXX: could propagate the exception */
    js_module_set_import_meta(ctx, func_val, TRUE, FALSE);
    /* the module is already referenced, so we must free it */
    m = JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);

    return m;
}

static char *tjs_find_module_ext(JSContext *ctx, const char *module_name)
{
    char module_path[PATH_MAX + 1];
    struct stat sb;

    if (stat(module_name, &sb) == 0 && S_ISREG(sb.st_mode))
        return js_strdup(ctx, module_name);

    snprintf(module_path, sizeof(module_path), "%s.js", module_name);
    if (stat(module_path, &sb) == 0 && S_ISREG(sb.st_mode))
        return js_strdup(ctx, module_path);

    snprintf(module_path, sizeof(module_path), "%s/index.js", module_name);
    if (stat(module_path, &sb) == 0 && S_ISREG(sb.st_mode))
        return js_strdup(ctx, module_path);

    snprintf(module_path, sizeof(module_path), "%s.so", module_name);
    if (stat(module_path, &sb) == 0 && S_ISREG(sb.st_mode))
        return js_strdup(ctx, module_path);

    return NULL;
}

static char *tjs_search_module(JSContext *ctx, const char *module_name)
{
    char *module_path = NULL;
    const char *search_paths;
    const char *search_path;

    /* Search for module in colon-separated paths in tjs_LIBRARY_PATH. */
    search_paths = getenv("TJS_LIBRARY_PATH");
    if (search_paths == NULL)
        search_paths = TJS_LIBRARY_PATH_DEFAULT;

    for (search_path = search_paths; search_path && *search_path;) {
        char name[PATH_MAX + 1];
        const char *end;

        end = strchr(search_path, ':');
        if (!end)
            end = search_path + strlen(search_path);

        snprintf(name, sizeof(name), "%.*s/%s", (int)(end - search_path),
            search_path, module_name);
        module_path = tjs_find_module_ext(ctx, name);

        if (module_path != NULL)
            break;

        search_path = end + 1;
    }

    return module_path;
}

char *tjs_module_normalize_name(JSContext *ctx, const char *base_name,
    const char *name, void *data)
{
    const char cd_prefix[] = "./";
    const char ud_prefix[] = "../";
    const char abs_prefix[] = "/";

    if (strncmp(name, cd_prefix, sizeof(cd_prefix) - 1) == 0
            || strncmp(name, ud_prefix, sizeof(ud_prefix) - 1) == 0) {
        char path[PATH_MAX + 1];
        char *tmp, *dir;

        tmp = js_strdup(ctx, base_name);
        if (!tmp)
            return NULL;
        dir = dirname(tmp);

        /* Module name is relative to the base name. */
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        free(tmp);

        return tjs_find_module_ext(ctx, path) ?: js_strdup(ctx, path);

    } else if (strncmp(name, abs_prefix, sizeof(abs_prefix) - 1) == 0) {
        /* Absolute path resolves directly. */
        return tjs_find_module_ext(ctx, name) ?: js_strdup(ctx, name);

    } else {
        /* Search for others in the library paths, or otherwise
         * return a copy of the name in case it's a built-in module
         * like "std" or "os". */
        return tjs_search_module(ctx, name) ?: js_strdup(ctx, name);
    }
}

#if defined(_WIN32)
#define TJS__PATHSEP  '\\'
#else
#define TJS__PATHSEP  '/'
#endif

int js_module_set_import_meta(JSContext *ctx, JSValueConst func_val, JS_BOOL use_realpath, JS_BOOL is_main) {
    JSModuleDef *m;
    char buf[PATH_MAX + 16] = {0};
    int r;
    JSValue meta_obj;
    JSAtom module_name_atom;
    const char *module_name;
    char module_dirname[PATH_MAX] = {0};
    char module_basename[PATH_MAX] = {0};

    CHECK_EQ(JS_VALUE_GET_TAG(func_val), JS_TAG_MODULE);
    m = JS_VALUE_GET_PTR(func_val);

    module_name_atom = JS_GetModuleName(ctx, m);
    module_name = JS_AtomToCString(ctx, module_name_atom);
#if 0
    fprintf(stdout, "XXX loaded module: %s\n", module_name);
#endif
    JS_FreeAtom(ctx, module_name_atom);
    if (!module_name)
        return -1;

    /* realpath() cannot be used with builtin modules
        because the corresponding module source code is not
        necessarily present */
    if (use_realpath) {
        uv_fs_t req;
        r = uv_fs_realpath(NULL, &req, module_name, NULL);
        if (r != 0) {
            uv_fs_req_cleanup(&req);
            JS_ThrowTypeError(ctx, "realpath failure");
            JS_FreeCString(ctx, module_name);
            return -1;
        }
        pstrcpy(buf, sizeof(buf), "file://");
        pstrcat(buf, sizeof(buf), req.ptr);
        uv_fs_req_cleanup(&req);

        // When using realpath we have the opportunity to extract the dirname
        // and basename and add them to the meta. Since the path is now absolute
        // all we need to do is split on the last path separator.
        const char *start = buf + 7; /* skip file:// */
        char *p = strrchr(start, TJS__PATHSEP);
        strncpy(module_dirname, start , p - start);
        strcpy(module_basename, p + 1);
    } else {
        pstrcat(buf, sizeof(buf), module_name);
    }

    JS_FreeCString(ctx, module_name);

    meta_obj = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta_obj))
        return -1;
    JS_DefinePropertyValueStr(ctx, meta_obj, "url", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta_obj, "main", JS_NewBool(ctx, is_main), JS_PROP_C_W_E);
    if (use_realpath) {
        JS_DefinePropertyValueStr(ctx, meta_obj, "dirname", JS_NewString(ctx, module_dirname), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, meta_obj, "basename", JS_NewString(ctx, module_basename), JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, meta_obj);
    return 0;
}

#undef TJS__PATHSEP
