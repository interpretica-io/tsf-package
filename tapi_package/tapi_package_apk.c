/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief apk backend
 *
 * Alpine: @c apk for everything.
 */

#define TE_LGR_USER "TAPI PACKAGE APK"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

static te_errno
apk_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "update", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "apk", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
apk_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[4];
    size_t n = 0;
    bool is_file = (n_names == 1 && strstr(names[0], ".apk") != NULL);

    head[n++] = "add";
    if (is_file)
        head[n++] = "--allow-untrusted";  /* a local file has no index sig */
    if (ctx->offline)
        head[n++] = "--no-network";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "apk", head, names, n_names, result);
}

static te_errno
apk_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char **names, size_t n_names, bool purge,
           tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;

    head[n++] = "del";
    if (purge)
        head[n++] = "--purge";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "apk", head, names, n_names, result);
}

/** Split "name-1.2.3-r0" into name and version at the version boundary:
 *  the first "-" followed by a digit. */
static bool
apk_split_nv(const char *nv, size_t len, char **name, char **version)
{
    size_t i;

    for (i = 0; i + 1 < len; i++)
    {
        if (nv[i] == '-' && nv[i + 1] >= '0' && nv[i + 1] <= '9')
        {
            *name = TE_ALLOC(i + 1);
            memcpy(*name, nv, i);
            *version = TE_ALLOC(len - i);
            memcpy(*version, nv + i + 1, len - i - 1);
            return true;
        }
    }

    return false;
}

static te_errno
apk_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          const char *name, tapi_package_info *pkg)
{
    const char *ev_args[] = { "info", "-e", name };
    const char *v_args[] = { "info", "-v", name };
    te_string ev = TE_STRING_INIT;
    te_string vv = TE_STRING_INIT;
    int status;
    size_t nlen = strlen(name);
    const char *pos;
    te_errno rc;

    /* -e prints the name if installed, nothing if not. */
    rc = tapi_package_query_raw(factory, ctx, "apk", ev_args,
                                TE_ARRAY_LEN(ev_args), &ev, &status);
    if (rc != 0)
        goto out;
    if (ev.ptr == NULL || ev.len == 0)
        goto out;

    pkg->installed = true;

    /* -v prints "name-version ..." lines; take the one for this name. */
    rc = tapi_package_query_raw(factory, ctx, "apk", v_args,
                                TE_ARRAY_LEN(v_args), &vv, &status);
    if (rc != 0)
        goto out;

    for (pos = vv.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = memchr(pos, ' ', len);
        size_t nvlen = (sp != NULL) ? (size_t)(sp - pos) : len;
        char *pn = NULL;
        char *pv = NULL;

        if (nvlen > nlen && strncmp(pos, name, nlen) == 0 &&
            pos[nlen] == '-' &&
            apk_split_nv(pos, nvlen, &pn, &pv))
        {
            if (strcmp(pn, name) == 0)
            {
                pkg->version = pv;
                free(pn);
                break;
            }
            free(pn);
            free(pv);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&ev);
    te_string_free(&vv);

    return rc;
}

static te_errno
apk_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
         te_vec *packages)
{
    const char *args[] = { "info", "-v" };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "apk", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = memchr(pos, ' ', len);
        size_t nvlen = (sp != NULL) ? (size_t)(sp - pos) : len;
        char *pn = NULL;
        char *pv = NULL;

        if (nvlen > 0 && apk_split_nv(pos, nvlen, &pn, &pv))
        {
            tapi_package_info info;

            memset(&info, 0, sizeof(info));
            info.installed = true;
            info.name = pn;
            info.version = pv;
            TE_VEC_APPEND(packages, info);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
apk_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          const char *name, te_vec *paths)
{
    const char *args[] = { "info", "-L", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "apk", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;
    if (status != 0)
    {
        rc = TE_RC(TE_TAPI, TE_ENOENT);
        goto out;
    }

    /* apk prints a header line "name-ver contains:" then bare paths. */
    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);

        if (len > 0 && pos[0] != ' ' && memchr(pos, ':', len) == NULL)
        {
            char *path = TE_ALLOC(len + 2);

            path[0] = '/';
            memcpy(path + 1, pos, len);
            TE_VEC_APPEND(paths, path);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

const tapi_package_backend tapi_package_backend_apk = {
    .manager = TAPI_PACKAGE_APK,
    .tool = "apk",
    .update = apk_update,
    .install = apk_install,
    .remove = apk_remove,
    .query = apk_query,
    .list = apk_list,
    .files = apk_files,
    .verify = NULL,     /* apk has no per-file verify command */
};
