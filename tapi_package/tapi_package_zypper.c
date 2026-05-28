/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief zypper backend
 *
 * SUSE: @c zypper to change, @c rpm (shared) to ask.
 */

#define TE_LGR_USER "TAPI PACKAGE ZYPPER"

#include "te_config.h"

#include "te_defs.h"
#include "te_errno.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

static te_errno
zypper_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const char *head[] = { "--non-interactive", "refresh", NULL };
    tapi_package_result result;
    te_errno rc;

    if (ctx->offline)
        return 0;

    rc = tapi_package_op(factory, ctx, "zypper", head, NULL, 0, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);

    return rc;
}

static te_errno
zypper_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
               const char **names, size_t n_names, tapi_package_result *result)
{
    const char *head[4];
    size_t n = 0;

    if (ctx->assume_yes)
        head[n++] = "--non-interactive";
    head[n++] = "install";
    if (ctx->offline)
        head[n++] = "--no-refresh";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "zypper", head, names, n_names,
                           result);
}

static te_errno
zypper_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
              const char **names, size_t n_names, bool purge,
              tapi_package_result *result)
{
    const char *head[3];
    size_t n = 0;

    (void)purge;
    if (ctx->assume_yes)
        head[n++] = "--non-interactive";
    head[n++] = "remove";
    head[n] = NULL;

    return tapi_package_op(factory, ctx, "zypper", head, names, n_names,
                           result);
}

const tapi_package_backend tapi_package_backend_zypper = {
    .manager = TAPI_PACKAGE_ZYPPER,
    .tool = "zypper",
    .update = zypper_update,
    .install = zypper_install,
    .remove = zypper_remove,
    .query = tapi_package_rpm_query,
    .list = tapi_package_rpm_list,
    .files = tapi_package_rpm_files,
    .verify = tapi_package_rpm_verify,
};
