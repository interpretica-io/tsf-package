/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief brew backend
 *
 * macOS and Linuxbrew: @c brew. brew refuses to run as root, so this
 * backend never prefixes @c sudo whatever the context says.
 */

#define TE_LGR_USER "TAPI PACKAGE BREW"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

/** Run brew directly (never through sudo) and fill a result. */
static te_errno
brew_op(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
        const char **head, const char **names, size_t n_names,
        tapi_package_result *result)
{
    te_vec args = TE_VEC_INIT(const char *);
    int timeout = ctx->timeout_ms > 0 ? ctx->timeout_ms :
                                        TAPI_PACKAGE_TIMEOUT_MS;
    int status = -1;
    size_t i;
    te_errno rc;

    tapi_package_result_init(result);

    for (i = 0; head != NULL && head[i] != NULL; i++)
        TE_VEC_APPEND(&args, head[i]);
    for (i = 0; i < n_names; i++)
        TE_VEC_APPEND(&args, names[i]);

    rc = tapi_package_run(factory, "brew", "brew",
                          (const char **)te_vec_get_mutable(&args, 0),
                          te_vec_size(&args), timeout, &result->output,
                          &result->output, &status);
    te_vec_free(&args);
    if (rc != 0)
        return rc;

    result->status = status;
    result->ok = (status == 0);
    if (!result->ok)
        ERROR("brew failed (exit %d): %s", status,
              result->output.ptr != NULL ? result->output.ptr : "");

    return 0;
}

static te_errno
brew_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "update", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = brew_op(factory, ctx, head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
brew_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
             const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[] = { "install", NULL };

    return brew_op(factory, ctx, head, names, n_names, result);
}

static te_errno
brew_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            const char **names, size_t n_names, bool purge,
            tapi_package_result *result)
{
    const char *head[] = { "uninstall", NULL };

    (void)purge;

    return brew_op(factory, ctx, head, names, n_names, result);
}

static te_errno
brew_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char *name, tapi_package_info *pkg)
{
    const char *args[] = { "list", "--versions", name };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    /* "name 1.2.3" when installed, empty and exit 1 when not. */
    rc = tapi_package_query_raw(factory, ctx, "brew", args,
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
brew_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          te_vec *packages)
{
    const char *args[] = { "list", "--versions" };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "brew", args,
                                TE_ARRAY_LEN(args), &out, &status);
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
            /* A formula can list several versions; keep the first. */
            {
                const char *vend = memchr(sp + 1, ' ', pos + len - sp - 1);
                size_t vlen = (vend != NULL) ? (size_t)(vend - sp - 1) :
                                               (size_t)(pos + len - sp - 1);

                info.version = TE_ALLOC(vlen + 1);
                memcpy(info.version, sp + 1, vlen);
            }
            TE_VEC_APPEND(packages, info);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
brew_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char *name, te_vec *paths)
{
    const char *args[] = { "list", name };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    /* For an installed formula, "brew list name" prints its files. */
    rc = tapi_package_query_raw(factory, ctx, "brew", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc == 0 && status != 0)
        rc = TE_RC(TE_TAPI, TE_ENOENT);
    if (rc == 0)
        tapi_package_lines_to_vec(out.ptr != NULL ? out.ptr : "", paths);

    te_string_free(&out);

    return rc;
}

const tapi_package_backend tapi_package_backend_brew = {
    .manager = TAPI_PACKAGE_BREW,
    .tool = "brew",
    .update = brew_update,
    .install = brew_install,
    .remove = brew_remove,
    .query = brew_query,
    .list = brew_list,
    .files = brew_files,
    .verify = NULL,
};
