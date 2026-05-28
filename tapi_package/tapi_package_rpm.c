/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief rpm query helpers
 *
 * The read-only side of the dnf and zypper backends: both ask @c rpm the
 * same way, so query, list, files and verify live here once.
 */

#define TE_LGR_USER "TAPI PACKAGE RPM"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

/* See description in tapi_package_internal.h */
te_errno
tapi_package_rpm_query(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char *name,
                       tapi_package_info *pkg)
{
    const char *args[] = {
        "-q", "--qf", "%{VERSION}-%{RELEASE}\t%{ARCH}\n", name,
    };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "rpm", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;

    if (status == 0 && out.ptr != NULL)
    {
        char *tab = strchr(out.ptr, '\t');
        char *eol = strchr(out.ptr, '\n');

        if (tab != NULL && eol != NULL)
        {
            *tab = *eol = '\0';
            pkg->installed = true;
            pkg->version = TE_STRDUP(out.ptr);
            pkg->arch = TE_STRDUP(tab + 1);
        }
    }

out:
    te_string_free(&out);

    return rc;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_rpm_list(tapi_job_factory_t *factory,
                      const tapi_package_ctx *ctx, te_vec *packages)
{
    const char *args[] = {
        "-qa", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\t%{ARCH}\n",
    };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "rpm", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        char line[512];
        char *tab1;
        char *tab2;

        if (len > 0 && len < sizeof(line))
        {
            memcpy(line, pos, len);
            line[len] = '\0';
            tab1 = strchr(line, '\t');
            tab2 = (tab1 != NULL) ? strchr(tab1 + 1, '\t') : NULL;
            if (tab1 != NULL && tab2 != NULL)
            {
                tapi_package_info info;

                *tab1 = *tab2 = '\0';
                memset(&info, 0, sizeof(info));
                info.installed = true;
                info.name = TE_STRDUP(line);
                info.version = TE_STRDUP(tab1 + 1);
                info.arch = TE_STRDUP(tab2 + 1);
                TE_VEC_APPEND(packages, info);
            }
        }
        pos = (eol != NULL) ? eol + 1 : NULL;
    }

out:
    te_string_free(&out);

    return rc;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_rpm_files(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char *name,
                       te_vec *paths)
{
    const char *args[] = { "-ql", name };
    te_string out = TE_STRING_INIT;
    int status;
    te_errno rc;

    rc = tapi_package_query_raw(factory, ctx, "rpm", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc == 0 && status != 0)
        rc = TE_RC(TE_TAPI, TE_ENOENT);
    if (rc == 0 && out.ptr != NULL &&
        strstr(out.ptr, "contains no files") == NULL)
        tapi_package_lines_to_vec(out.ptr, paths);

    te_string_free(&out);

    return rc;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_rpm_verify(tapi_job_factory_t *factory,
                        const tapi_package_ctx *ctx, const char *name,
                        te_vec *changed)
{
    const char *args[] = { "-V", name };
    te_string out = TE_STRING_INIT;
    const char *pos;
    int status;
    te_errno rc;

    /* rpm -V prints only files that no longer match; exit 1 if any do. */
    rc = tapi_package_query_raw(factory, ctx, "rpm", args, TE_ARRAY_LEN(args),
                                &out, &status);
    if (rc != 0)
        goto out;

    for (pos = out.ptr; pos != NULL && *pos != '\0'; )
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        const char *sp = pos;

        /* "SM5....T.  c /etc/foo": path is the last space-separated field. */
        while (sp < pos + len && *sp != ' ')
            sp++;
        while (sp < pos + len && *sp == ' ')
            sp++;
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
