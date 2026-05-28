/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Package managers of a Test Agent
 *
 * The core: run helpers, detection, context, the generic operations that
 * dispatch to a backend, and ensure/rollback.
 */

#define TE_LGR_USER "TAPI PACKAGE"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "tapi_job_opt.h"

#include "tapi_devtool_run.h"

#include "tapi_package.h"
#include "tapi_package_internal.h"

/** Arguments of a command, as a plain vector of strings. */
typedef struct pkg_cmd_opt {
    size_t n_args;
    const char **args;
} pkg_cmd_opt;

static const tapi_job_opt_bind pkg_cmd_binds[] = TAPI_JOB_OPT_SET(
    TAPI_JOB_OPT_ARRAY_PTR(pkg_cmd_opt, n_args, args,
        TAPI_JOB_OPT_CONTENT(TAPI_JOB_OPT_STRING, NULL, false))
);

/* See description in tapi_package_internal.h */
te_errno
tapi_package_run(tapi_job_factory_t *factory, const char *name,
                 const char *program, const char **args, size_t n_args,
                 int timeout_ms, te_string *out, te_string *err, int *status)
{
    pkg_cmd_opt opt = { .n_args = n_args, .args = args };
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    tapi_devtool_output output;
    te_errno rc;

    rc = tapi_devtool_run_init(&run, factory, name, program, pkg_cmd_binds,
                               &opt, NULL);
    if (rc != 0)
        return rc;

    rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);

    if (rc == 0)
    {
        tapi_devtool_run_get_output(&run, &output);
        if (out != NULL)
            te_string_append(out, "%s", output.out);
        if (err != NULL)
            te_string_append(err, "%s", output.err);
        if (status != NULL)
            *status = (output.status.type == TAPI_JOB_STATUS_EXITED) ?
                      output.status.value : -1;
    }

    tapi_devtool_run_fini(&run);

    return rc;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_sh(tapi_job_factory_t *factory, const char *name,
                const char *script, const char **args, size_t n_args,
                int timeout_ms, te_string *out, te_string *err, int *status)
{
    const char **argv;
    size_t i;
    te_errno rc;

    argv = TE_ALLOC((n_args + 3) * sizeof(*argv));
    argv[0] = "-c";
    argv[1] = script;
    argv[2] = "sh";
    for (i = 0; i < n_args; i++)
        argv[i + 3] = args[i];

    rc = tapi_package_run(factory, name, "sh", argv, n_args + 3, timeout_ms,
                          out, err, status);

    free(argv);

    return rc;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_have_tool(tapi_job_factory_t *factory, const char *program,
                       bool *present)
{
    const char *args[] = { program };
    int status;
    te_errno rc;

    rc = tapi_package_sh(factory, "which", "command -v \"$1\" >/dev/null",
                         args, 1, TAPI_PACKAGE_QUERY_TIMEOUT_MS, NULL, NULL,
                         &status);
    if (rc == 0)
        *present = (status == 0);

    return rc;
}

/* See description in tapi_package_internal.h */
void
tapi_package_lines_to_vec(const char *text, te_vec *vec)
{
    const char *pos = text;

    while (pos != NULL && *pos != '\0')
    {
        const char *eol = strchr(pos, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - pos) : strlen(pos);
        char *line;

        while (len > 0 && (pos[len - 1] == '\r' || pos[len - 1] == ' '))
            len--;

        if (len > 0)
        {
            line = TE_ALLOC(len + 1);
            memcpy(line, pos, len);
            TE_VEC_APPEND(vec, line);
        }

        pos = (eol != NULL) ? eol + 1 : NULL;
    }
}

/* See description in tapi_package.h */
const char *
tapi_package_manager2str(tapi_package_manager manager)
{
    switch (manager)
    {
        case TAPI_PACKAGE_APT:    return "apt";
        case TAPI_PACKAGE_DNF:    return "dnf";
        case TAPI_PACKAGE_ZYPPER: return "zypper";
        case TAPI_PACKAGE_PACMAN: return "pacman";
        case TAPI_PACKAGE_APK:    return "apk";
        case TAPI_PACKAGE_OPKG:   return "opkg";
        case TAPI_PACKAGE_BREW:   return "brew";
        case TAPI_PACKAGE_NONE:   return "none";
    }

    return "unknown";
}

/** The backends, in the order detection tries them. */
static const tapi_package_backend *const backends[] = {
    &tapi_package_backend_apt,
    &tapi_package_backend_dnf,
    &tapi_package_backend_zypper,
    &tapi_package_backend_pacman,
    &tapi_package_backend_apk,
    &tapi_package_backend_opkg,
    &tapi_package_backend_brew,
};

/* See description in tapi_package_internal.h */
const tapi_package_backend *
tapi_package_backend_get(tapi_package_manager manager)
{
    size_t i;

    for (i = 0; i < TE_ARRAY_LEN(backends); i++)
    {
        if (backends[i]->manager == manager)
            return backends[i];
    }

    return NULL;
}

/* See description in tapi_package.h */
te_errno
tapi_package_detect(tapi_job_factory_t *factory,
                    tapi_package_manager *manager)
{
    size_t i;

    *manager = TAPI_PACKAGE_NONE;

    for (i = 0; i < TE_ARRAY_LEN(backends); i++)
    {
        bool present = false;
        te_errno rc;

        rc = tapi_package_have_tool(factory, backends[i]->tool, &present);
        if (rc != 0)
            return rc;
        if (present)
        {
            *manager = backends[i]->manager;
            RING("Package manager on the agent: %s (%s)",
                 tapi_package_manager2str(*manager), backends[i]->tool);
            return 0;
        }
    }

    WARN("No known package manager on the agent");

    return TE_RC(TE_TAPI, TE_ENOENT);
}

/* See description in tapi_package.h */
te_errno
tapi_package_ctx_init(tapi_job_factory_t *factory, tapi_package_ctx *ctx)
{
    te_errno rc;

    memset(ctx, 0, sizeof(*ctx));
    ctx->assume_yes = true;
    ctx->sudo = false;
    ctx->timeout_ms = TAPI_PACKAGE_TIMEOUT_MS;
    ctx->installed_by_us = (te_vec)TE_VEC_INIT_AUTOPTR(char *);
    ctx->removed_by_us = (te_vec)TE_VEC_INIT_AUTOPTR(char *);

    rc = tapi_package_detect(factory, &ctx->manager);
    if (rc != 0)
    {
        te_vec_free(&ctx->installed_by_us);
        te_vec_free(&ctx->removed_by_us);
    }

    return rc;
}

/* See description in tapi_package.h */
void
tapi_package_ctx_fini(tapi_package_ctx *ctx)
{
    te_vec_free(&ctx->installed_by_us);
    te_vec_free(&ctx->removed_by_us);
    ctx->manager = TAPI_PACKAGE_NONE;
}

/** Get the backend of a context, or fail with a clear message. */
static te_errno
ctx_backend(const tapi_package_ctx *ctx, const tapi_package_backend **backend)
{
    *backend = tapi_package_backend_get(ctx->manager);
    if (*backend == NULL)
    {
        ERROR("No backend for package manager %s",
              tapi_package_manager2str(ctx->manager));
        return TE_RC(TE_TAPI, TE_ENOENT);
    }

    return 0;
}

/* See description in tapi_package.h */
void
tapi_package_result_init(tapi_package_result *result)
{
    result->ok = false;
    result->status = -1;
    result->output = (te_string)TE_STRING_INIT;
}

/* See description in tapi_package.h */
void
tapi_package_result_free(tapi_package_result *result)
{
    te_string_free(&result->output);
}

/* See description in tapi_package.h */
void
tapi_package_info_free(tapi_package_info *pkg)
{
    free(pkg->name);
    free(pkg->version);
    free(pkg->arch);
    pkg->name = NULL;
    pkg->version = NULL;
    pkg->arch = NULL;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_op(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                const char *tool, const char **head, const char **names,
                size_t n_names, tapi_package_result *result)
{
    te_vec args = TE_VEC_INIT(const char *);
    const char *program = tool;
    int timeout = ctx->timeout_ms > 0 ? ctx->timeout_ms :
                                        TAPI_PACKAGE_TIMEOUT_MS;
    int status = -1;
    size_t i;
    te_errno rc;

    tapi_package_result_init(result);

    if (ctx->sudo)
    {
        const char *s = tool;

        program = "sudo";
        TE_VEC_APPEND(&args, s);
    }
    for (i = 0; head != NULL && head[i] != NULL; i++)
        TE_VEC_APPEND(&args, head[i]);
    for (i = 0; i < n_names; i++)
        TE_VEC_APPEND(&args, names[i]);

    rc = tapi_package_run(factory, tool, program,
                          (const char **)te_vec_get_mutable(&args, 0),
                          te_vec_size(&args), timeout, &result->output,
                          &result->output, &status);
    te_vec_free(&args);
    if (rc != 0)
        return rc;

    result->status = status;
    result->ok = (status == 0);
    if (!result->ok)
        ERROR("%s failed (exit %d): %s", tool, status,
              result->output.ptr != NULL ? result->output.ptr : "");

    return 0;
}

/* See description in tapi_package_internal.h */
te_errno
tapi_package_query_raw(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char *tool,
                       const char **args, size_t n_args, te_string *out,
                       int *status)
{
    (void)ctx;

    return tapi_package_run(factory, tool, tool, args, n_args,
                            TAPI_PACKAGE_QUERY_TIMEOUT_MS, out, NULL, status);
}

/* See description in tapi_package.h */
te_errno
tapi_package_update(tapi_job_factory_t *factory, const tapi_package_ctx *ctx)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;
    if (backend->update == NULL)
        return 0;

    return backend->update(factory, ctx);
}

/* See description in tapi_package.h */
te_errno
tapi_package_install(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                     const char **names, size_t n_names,
                     tapi_package_result *result)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;

    return backend->install(factory, ctx, names, n_names, result);
}

/* See description in tapi_package.h */
te_errno
tapi_package_install_file(tapi_job_factory_t *factory,
                          const tapi_package_ctx *ctx, const char *path,
                          tapi_package_result *result)
{
    const tapi_package_backend *backend;
    const char *names[1] = { path };
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;

    /*
     * apt, dnf, zypper, apk and opkg all install a local file when the
     * argument looks like a path; brew and pacman need a different verb,
     * handled in their backends. The generic path is install() with the
     * file name, which the backends that need a special verb override by
     * spotting a path.
     */
    return backend->install(factory, ctx, names, 1, result);
}

/* See description in tapi_package.h */
te_errno
tapi_package_remove(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                    const char **names, size_t n_names, bool purge,
                    tapi_package_result *result)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;

    return backend->remove(factory, ctx, names, n_names, purge, result);
}

/* See description in tapi_package.h */
te_errno
tapi_package_query(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                   const char *name, tapi_package_info *pkg)
{
    const tapi_package_backend *backend;
    te_errno rc;

    memset(pkg, 0, sizeof(*pkg));
    pkg->name = TE_STRDUP(name);

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
    {
        tapi_package_info_free(pkg);
        return rc;
    }

    return backend->query(factory, ctx, name, pkg);
}

/* See description in tapi_package.h */
te_errno
tapi_package_installed(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char *name,
                       bool *installed)
{
    tapi_package_info pkg;
    te_errno rc;

    rc = tapi_package_query(factory, ctx, name, &pkg);
    if (rc == 0)
        *installed = pkg.installed;
    tapi_package_info_free(&pkg);

    return rc;
}

/* See description in tapi_package.h */
te_errno
tapi_package_list(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                  te_vec *packages)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;

    return backend->list(factory, ctx, packages);
}

/* See description in tapi_package.h */
void
tapi_package_list_free(te_vec *packages)
{
    tapi_package_info *pkg;

    TE_VEC_FOREACH(packages, pkg)
        tapi_package_info_free(pkg);

    te_vec_free(packages);
}

/* See description in tapi_package.h */
te_errno
tapi_package_files(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                   const char *name, te_vec *paths)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;
    if (backend->files == NULL)
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);

    return backend->files(factory, ctx, name, paths);
}

/* See description in tapi_package.h */
te_errno
tapi_package_verify(tapi_job_factory_t *factory, const tapi_package_ctx *ctx,
                    const char *name, te_vec *changed)
{
    const tapi_package_backend *backend;
    te_errno rc;

    rc = ctx_backend(ctx, &backend);
    if (rc != 0)
        return rc;
    if (backend->verify == NULL)
    {
        WARN("%s cannot verify a package's files",
             tapi_package_manager2str(ctx->manager));
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    return backend->verify(factory, ctx, name, changed);
}

/* See description in tapi_package.h */
te_errno
tapi_package_ensure(tapi_job_factory_t *factory, tapi_package_ctx *ctx,
                    const char *name)
{
    tapi_package_result result;
    bool installed = false;
    char *record;
    te_errno rc;

    rc = tapi_package_installed(factory, ctx, name, &installed);
    if (rc != 0)
        return rc;
    if (installed)
    {
        RING("%s is already installed; leaving it", name);
        return 0;
    }

    rc = tapi_package_install(factory, ctx, &name, 1, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);
    if (rc != 0)
        return rc;

    record = TE_STRDUP(name);
    TE_VEC_APPEND(&ctx->installed_by_us, record);

    return 0;
}

/* See description in tapi_package.h */
te_errno
tapi_package_ensure_absent(tapi_job_factory_t *factory,
                           tapi_package_ctx *ctx, const char *name)
{
    tapi_package_result result;
    bool installed = false;
    char *record;
    te_errno rc;

    rc = tapi_package_installed(factory, ctx, name, &installed);
    if (rc != 0)
        return rc;
    if (!installed)
        return 0;

    rc = tapi_package_remove(factory, ctx, &name, 1, false, &result);
    if (rc == 0 && !result.ok)
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    tapi_package_result_free(&result);
    if (rc != 0)
        return rc;

    record = TE_STRDUP(name);
    TE_VEC_APPEND(&ctx->removed_by_us, record);

    return 0;
}

/* See description in tapi_package.h */
te_errno
tapi_package_ctx_rollback(tapi_job_factory_t *factory, tapi_package_ctx *ctx)
{
    tapi_package_result result;
    char * const *name;
    te_errno first = 0;

    /* Remove what we installed. */
    TE_VEC_FOREACH(&ctx->installed_by_us, name)
    {
        te_errno rc;

        rc = tapi_package_remove(factory, ctx, (const char **)name, 1, false,
                                 &result);
        if (rc == 0 && !result.ok)
            rc = TE_RC(TE_TAPI, TE_EFAIL);
        tapi_package_result_free(&result);
        if (rc != 0 && first == 0)
            first = rc;
    }

    /* Put back what we removed. */
    TE_VEC_FOREACH(&ctx->removed_by_us, name)
    {
        te_errno rc;

        rc = tapi_package_install(factory, ctx, (const char **)name, 1,
                                  &result);
        if (rc == 0 && !result.ok)
            rc = TE_RC(TE_TAPI, TE_EFAIL);
        tapi_package_result_free(&result);
        if (rc != 0 && first == 0)
            first = rc;
    }

    te_vec_reset(&ctx->installed_by_us);
    te_vec_reset(&ctx->removed_by_us);

    return first;
}
