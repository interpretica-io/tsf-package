/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief dnf backend
 *
 * Fedora and RHEL: @c dnf to change, @c rpm (shared) to ask.
 */

#define TE_LGR_USER "TAPI PACKAGE DNF"

#include "te_config.h"

#include "te_defs.h"
#include "te_errno.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

static te_errno
dnf_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "-y", "makecache", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "dnf", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
dnf_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
            const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[4];
    size_t n = 0;

    head[n++] = "install";
    if (ctx->assume_yes)
        head[n++] = "-y";
    if (ctx->offline)
        head[n++] = "--cacheonly";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "dnf", head, names, n_names, result);
}

static te_errno
dnf_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
           const char **names, size_t n_names, bool purge,
           tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;

    (void)purge;    /* rpm has no separate config to purge. */
    head[n++] = "remove";
    if (ctx->assume_yes)
        head[n++] = "-y";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "dnf", head, names, n_names, result);
}

const tapi_package_backend tapi_package_backend_dnf = {
    .manager = TAPI_PACKAGE_DNF,
    .tool = "dnf",
    .update = dnf_update,
    .install = dnf_install,
    .remove = dnf_remove,
    .query = tapi_package_rpm_query,
    .list = tapi_package_rpm_list,
    .files = tapi_package_rpm_files,
    .verify = tapi_package_rpm_verify,
};
