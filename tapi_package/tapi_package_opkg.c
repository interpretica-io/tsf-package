/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief opkg backend
 *
 * OpenWrt and other embedded systems: @c opkg for everything. opkg has
 * no per-file verify command, so verify is not offered.
 */

#define TE_LGR_USER "TAPI PACKAGE OPKG"

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
opkg_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "update", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "opkg", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
opkg_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
             const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[2];
    size_t n = 0;

    /* opkg installs a local .ipk when the argument is a path; same verb. */
    head[n++] = "install";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "opkg", head, names, n_names, result);
}

static te_errno
opkg_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            const char **names, size_t n_names, bool purge,
            tapi_package_result *result)
{
    const char *head[2];
    size_t n = 0;

    (void)purge;
    head[n++] = "remove";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "opkg", head, names, n_names, result);
}

static te_errno
opkg_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char *name, tapi_package_info *pkg)
{
    const char *args[] = { "status", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /*
     * "opkg status name" prints a stanza with Package:, Version: and
     * Status: lines when the package is known; an installed one has
     * "Status: ... installed".
     */
    rc = tapi_package_query_raw(factory, ctx, "opkg", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0 || out.ptr == NULL)
        goto out;

    if (strstr(out.ptr, "Status:") == NULL ||
        strstr(out.ptr, "installed") == NULL)
        goto out;

    pkg->installed = true;
    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);

        if (len > 9 && strncmp(pos, "Version: ", 9) == 0)
        {
            pkg->version = TE_ALLOC(len - 9 + 1);
            memcpy(pkg->version, pos + 9, len - 9);
            break;
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
opkg_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          te_vec *packages)
{
    const char *args[] = { "list-installed" };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /* Each line is "name - version". */
    rc = tapi_package_query_raw(factory, ctx, "opkg", args, 1, &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sep = NULL;
        const char *p;

        for (p = pos; p + 2 < pos + len; p++)
        {
            if (p[0] == ' ' && p[1] == '-' && p[2] == ' ')
            {
                sep = p;
                break;
            }
        }
        if (sep != NULL)
        {
            tapi_package_info info;

            memset(&info, 0, sizeof(info));
            info.installed = true;
            info.name = TE_ALLOC(sep - pos + 1);
            memcpy(info.name, pos, sep - pos);
            info.version = TE_ALLOC(pos + len - (sep + 3) + 1);
            memcpy(info.version, sep + 3, pos + len - (sep + 3));
            TE_VEC_APPEND(packages, info);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
opkg_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char *name, te_vec *paths)
{
    const char *args[] = { "files", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /* First line is a header "Package X ... has following files:". */
    rc = tapi_package_query_raw(factory, ctx, "opkg", args, TE_ARRAY_LEN(args),
                                &out, &status);
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

        if (len > 0 && pos[0] == '/')
        {
            char *path = TE_ALLOC(len + 1);

            memcpy(path, pos, len);
            TE_VEC_APPEND(paths, path);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

const tapi_package_backend tapi_package_backend_opkg = {
    .manager = TAPI_PACKAGE_OPKG,
    .tool = "opkg",
    .update = opkg_update,
    .install = opkg_install,
    .remove = opkg_remove,
    .query = opkg_query,
    .list = opkg_list,
    .files = opkg_files,
    .verify = NULL,
};
