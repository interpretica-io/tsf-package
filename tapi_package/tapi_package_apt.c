/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief apt backend
 *
 * Debian and Ubuntu: @c apt-get to change, @c dpkg-query and @c dpkg to
 * ask.
 */

#define TE_LGR_USER "TAPI PACKAGE APT"

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
apt_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "update", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "apt-get", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
apt_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[5];
    size_t n = 0;

    head[n++] = "install";
    head[n++] = ctx->assume_yes ? "-y" : "--simulate";
    if (ctx->offline)
        head[n++] = "--no-download";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "apt-get", head, names, n_names,
                           result);
}

static te_errno
apt_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char **names, size_t n_names, bool purge,
           tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;

    head[n++] = purge ? "purge" : "remove";
    head[n++] = ctx->assume_yes ? "-y" : "--simulate";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "apt-get", head, names, n_names,
                           result);
}

static te_errno
apt_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          const char *name, tapi_package_info *pkg)
{
    const char *args[] = {
        "-W", "-f=${Version}\t${Architecture}\t${db:Status-Status}\n", name,
    };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "dpkg-query", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc != 0)
        goto out;

    if (status == 0 && out.ptr != NULL)
    {
        char *ver = out.ptr;
        char *tab1 = strchr(ver, '\t');
        char *tab2 = (tab1 != NULL) ? strchr(tab1 + 1, '\t') : NULL;
        char *eol = strchr(ver, '\n');

        if (tab1 != NULL && tab2 != NULL && eol != NULL)
        {
            *tab1 = *tab2 = *eol = '\0';
            /* "installed" as the status word; "config-files" etc. are not. */
            if (strcmp(tab2 + 1, "installed") == 0)
            {
                pkg->installed = true;
                pkg->version = TE_STRDUP(ver);
                pkg->arch = TE_STRDUP(tab1 + 1);
            }
        }
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
apt_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
         te_vec *packages)
{
    const char *args[] = {
        "-W", "-f=${db:Status-Status}\t${Package}\t${Version}\t"
              "${Architecture}\n",
    };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "dpkg-query", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        char line[512];
        char *f[4];
        size_t nf = 0;
        char *p;

        if (len > 0 && len < sizeof(line))
        {
            memcpy(line, pos, len);
            line[len] = '\0';
            for (p = line, nf = 0; nf < 4; nf++)
            {
                char *tab = strchr(p, '\t');

                f[nf] = p;
                if (tab == NULL)
                    break;
                *tab = '\0';
                p = tab + 1;
            }
            if (nf == 3 && strcmp(f[0], "installed") == 0)
            {
                tapi_package_info info;

                memset(&info, 0, sizeof(info));
                info.installed = true;
                info.name = TE_STRDUP(f[1]);
                info.version = TE_STRDUP(f[2]);
                info.arch = TE_STRDUP(f[3]);
                TE_VEC_APPEND(packages, info);
            }
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

static te_errno
apt_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
          const char *name, te_vec *paths)
{
    const char *args[] = { "-L", name };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "dpkg-query", args,
                                TE_ARRAY_LEN(args), &out, &status);
    if (rc == 0 && status != 0)
        rc = TE_RC(TE_TAPI, TE_ENOENT);
    if (rc == 0)
        tapi_package_lines_to_vec(out.ptr != NULL ? out.ptr : "", paths);

    te_string_free(&out);

    return rc;
}

static te_errno
apt_verify(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char *name, te_vec *changed)
{
    const char *args[] = { "-V", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /* dpkg -V lists only files that no longer match; exit 1 if any do. */
    rc = tapi_package_query_raw(factory, ctx, "dpkg", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = pos;

        /* "??5?????? c /etc/foo": the path is the last space-separated field. */
        while (sp < pos + len && *sp != ' ')
            sp++;
        while (sp < pos + len && *sp == ' ')
            sp++;
        /* An optional file-type letter and another space. */
        if (sp + 1 < pos + len && sp[1] == ' ')
            sp += 2;
        if (sp < pos + len)
        {
            char *path = TE_ALLOC(pos + len - sp + 1);

            memcpy(path, sp, pos + len - sp);
            TE_VEC_APPEND(changed, path);
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

const tapi_package_backend tapi_package_backend_apt = {
    .manager = TAPI_PACKAGE_APT,
    .tool = "apt-get",
    .update = apt_update,
    .install = apt_install,
    .remove = apt_remove,
    .query = apt_query,
    .list = apt_list,
    .files = apt_files,
    .verify = apt_verify,
};
