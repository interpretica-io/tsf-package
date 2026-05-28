/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief pacman backend
 *
 * Arch: @c pacman for everything.
 */

#define TE_LGR_USER "TAPI PACKAGE PACMAN"

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
pacman_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "-Sy", "--noconfirm", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "pacman", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
pacman_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
               const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;
    bool is_file = (n_names == 1 && strchr(names[0], '/') != NULL);

    /* A path is a local package file: -U rather than -S. */
    head[n++] = is_file ? "-U" : "-S";
    if (ctx->assume_yes)
        head[n++] = "--noconfirm";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "pacman", head, names, n_names,
                           result);
}

static te_errno
pacman_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
              const char **names, size_t n_names, bool purge,
              tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;

    /* -Rns also removes config and now-orphaned dependencies. */
    head[n++] = purge ? "-Rns" : "-R";
    if (ctx->assume_yes)
        head[n++] = "--noconfirm";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "pacman", head, names, n_names,
                           result);
}

static te_errno
pacman_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
             const char *name, tapi_package_info *pkg)
{
    const char *args[] = { "-Q", name };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    /* "name version" on one line, exit 0 when installed. */
    rc = tapi_package_query_raw(factory, ctx, "pacman", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc != 0)
        goto out;

    if (status == 0 && out.ptr != NULL)
    {
        char *sp = strchr(out.ptr, ' ');
        char *eol = strchr(out.ptr, '\n');

        if (sp != NULL && eol != NULL)
        {
            *eol = '\0';
            pkg->installed = true;
            pkg->version = TE_STRDUP(sp + 1);
        }
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
pacman_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            te_vec *packages)
{
    const char *args[] = { "-Q" };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "pacman", args, 1, &out,
                                &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = memchr(pos, ' ', len);

        if (sp != NULL)
        {
            tapi_package_info info;

            memset(&info, 0, sizeof(info));
            info.installed = true;
            info.name = TE_ALLOC(sp - pos + 1);
            memcpy(info.name, pos, sp - pos);
            info.version = TE_ALLOC(pos + len - sp);
            memcpy(info.version, sp + 1, pos + len - sp - 1);
            TE_VEC_APPEND(packages, info);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
pacman_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
             const char *name, te_vec *paths)
{
    const char *args[] = { "-Ql", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /* Each line is "name /path"; keep the path. */
    rc = tapi_package_query_raw(factory, ctx, "pacman", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc != 0)
        goto out;
    if (status != 0)
    {
        rc = TE_RC(TE_TAPI, TE_ENOENT);
        goto out;
    }

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = memchr(pos, ' ', len);

        if (sp != NULL && sp + 1 < pos + len)
        {
            char *path = TE_ALLOC(pos + len - sp);

            memcpy(path, sp + 1, pos + len - sp - 1);
            TE_VEC_APPEND(paths, path);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
pacman_verify(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
              const char *name, te_vec *changed)
{
    const char *args[] = { "-Qkk", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /*
     * -Qkk checks every file; a problem line is
     * "name: /path (message)". A line without ": /" is a summary.
     */
    rc = tapi_package_query_raw(factory, ctx, "pacman", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *slash = NULL;
        const char *p;

        for (p = pos; p + 2 < pos + len; p++)
        {
            if (p[0] == ':' && p[1] == ' ' && p[2] == '/')
            {
                slash = p + 2;
                break;
            }
        }
        if (slash != NULL)
        {
            const char *end = slash;
            char *path;

            while (end < pos + len && *end != ' ')
                end++;
            path = TE_ALLOC(end - slash + 1);
            memcpy(path, slash, end - slash);
            TE_VEC_APPEND(changed, path);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

const tapi_package_backend tapi_package_backend_pacman = {
    .manager = TAPI_PACKAGE_PACMAN,
    .tool = "pacman",
    .update = pacman_update,
    .install = pacman_install,
    .remove = pacman_remove,
    .query = pacman_query,
    .list = pacman_list,
    .files = pacman_files,
    .verify = pacman_verify,
};
