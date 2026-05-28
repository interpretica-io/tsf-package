/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Package TAPI: internal helpers
 *
 * Internal to tsf-package; not installed.
 *
 * Every package manager is driven the same way: run its command-line
 * tool on the agent, look at how it exited and parse what it printed.
 * tsf-devtool already knows how to run a tool and capture its output;
 * this is the shape on top of it, plus the small parsing helpers the
 * backends share.
 */

#ifndef __TSF_TAPI_PACKAGE_INTERNAL_H__
#define __TSF_TAPI_PACKAGE_INTERNAL_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#include "tapi_package.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run @p program with @p args on the agent, wait for it and capture what
 * it printed on both streams.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  name         Tool name for log messages.
 * @param[in]  program      Program name or path.
 * @param[in]  args         Arguments after @c argv[0] (may be @c NULL).
 * @param[in]  n_args       Number of @p args.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] out          String to append stdout to (may be @c NULL).
 * @param[out] err          String to append stderr to (may be @c NULL).
 * @param[out] status       Exit status, or @c -1 if the program did not
 *                          exit normally (may be @c NULL).
 *
 * @return Status code of running the program, not of the program.
 */
extern te_errno tapi_package_run(tapi_job_factory_t *factory,
                                 const char *name, const char *program,
                                 const char **args, size_t n_args,
                                 int timeout_ms, te_string *out,
                                 te_string *err, int *status);

/**
 * Run a shell script: @c sh @c -c @p script @c sh @p args. Values reach
 * the script as positional parameters, never pasted into its text.
 *
 * Parameters are those of tapi_package_run().
 *
 * @return Status code of running the script, not of the script.
 */
extern te_errno tapi_package_sh(tapi_job_factory_t *factory, const char *name,
                                const char *script, const char **args,
                                size_t n_args, int timeout_ms, te_string *out,
                                te_string *err, int *status);

/**
 * Check whether a program is on the agent, i.e. @c command @c -v.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  program      Program name.
 * @param[out] present      Where to save the answer.
 *
 * @return Status code.
 */
extern te_errno tapi_package_have_tool(tapi_job_factory_t *factory,
                                       const char *program, bool *present);

/**
 * The backend behind a manager: the operations it implements. A field
 * is @c NULL when the manager does not do that operation.
 */
typedef struct tapi_package_backend {
    /** Manager this backend is for. */
    tapi_package_manager manager;
    /** The command-line tool, e.g. @c "apt-get". */
    const char *tool;

    /** Bring the package index up to date. */
    te_errno (*update)(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx);
    /** Install or upgrade packages. */
    te_errno (*install)(tapi_job_factory_t *factory,
                        const tapi_package_ctx *ctx, const char **names,
                        size_t n_names, tapi_package_result *result);
    /** Remove packages. */
    te_errno (*remove)(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char **names,
                       size_t n_names, bool purge,
                       tapi_package_result *result);
    /** Query one installed package; sets @p pkg->installed. */
    te_errno (*query)(tapi_job_factory_t *factory,
                      const tapi_package_ctx *ctx, const char *name,
                      tapi_package_info *pkg);
    /** List every installed package. */
    te_errno (*list)(tapi_job_factory_t *factory,
                     const tapi_package_ctx *ctx, te_vec *packages);
    /** List the files a package owns. */
    te_errno (*files)(tapi_job_factory_t *factory,
                      const tapi_package_ctx *ctx, const char *name,
                      te_vec *paths);
    /** Verify the files of a package against the manager's own database. */
    te_errno (*verify)(tapi_job_factory_t *factory,
                       const tapi_package_ctx *ctx, const char *name,
                       te_vec *changed);
} tapi_package_backend;

/**
 * Get the backend for a manager.
 *
 * @param manager       Manager.
 *
 * @return The backend, or @c NULL if it is not one this library drives.
 */
extern const tapi_package_backend *tapi_package_backend_get(
                                        tapi_package_manager manager);

/** Backends, defined in their own files, gathered by tapi_package.c. */
extern const tapi_package_backend tapi_package_backend_apt;
extern const tapi_package_backend tapi_package_backend_dnf;
extern const tapi_package_backend tapi_package_backend_pacman;
extern const tapi_package_backend tapi_package_backend_apk;
extern const tapi_package_backend tapi_package_backend_opkg;
extern const tapi_package_backend tapi_package_backend_brew;
extern const tapi_package_backend tapi_package_backend_zypper;

/**
 * Run a manager command built from a fixed head and a list of package
 * names, and fill a result from how it went.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  ctx          Context (timeout, assume-yes, sudo...).
 * @param[in]  tool         The command-line tool.
 * @param[in]  head         Fixed arguments before the names (may be
 *                          @c NULL), @c NULL terminated.
 * @param[in]  names        Package names (may be @c NULL).
 * @param[in]  n_names      Number of @p names.
 * @param[out] result       Result; release with
 *                          tapi_package_result_free().
 *
 * @return Status code.
 */
extern te_errno tapi_package_op(tapi_job_factory_t *factory,
                                const tapi_package_ctx *ctx, const char *tool,
                                const char **head, const char **names,
                                size_t n_names, tapi_package_result *result);

/**
 * Run a manager query and return its stdout, without failing on a
 * non-zero exit (a query for an absent package exits non-zero).
 *
 * @param[in]  factory      Job factory.
 * @param[in]  ctx          Context.
 * @param[in]  tool         The command-line tool.
 * @param[in]  args         Arguments after @c argv[0].
 * @param[in]  n_args       Number of @p args.
 * @param[out] out          String to append stdout to.
 * @param[out] status       Exit status (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_package_query_raw(tapi_job_factory_t *factory,
                                       const tapi_package_ctx *ctx,
                                       const char *tool, const char **args,
                                       size_t n_args, te_string *out,
                                       int *status);

/**
 * Query one RPM package (@c rpm @c -q). Shared by the dnf and zypper
 * backends. Sets @p pkg->installed, and its version and arch when it is.
 *
 * @return Status code.
 */
extern te_errno tapi_package_rpm_query(tapi_job_factory_t *factory,
                                       const tapi_package_ctx *ctx,
                                       const char *name,
                                       tapi_package_info *pkg);

/**
 * List every installed RPM package (@c rpm @c -qa). Shared.
 *
 * @return Status code.
 */
extern te_errno tapi_package_rpm_list(tapi_job_factory_t *factory,
                                      const tapi_package_ctx *ctx,
                                      te_vec *packages);

/**
 * List the files an RPM package owns (@c rpm @c -ql). Shared.
 *
 * @return Status code.
 */
extern te_errno tapi_package_rpm_files(tapi_job_factory_t *factory,
                                       const tapi_package_ctx *ctx,
                                       const char *name, te_vec *paths);

/**
 * Verify an RPM package (@c rpm @c -V). Shared.
 *
 * @return Status code.
 */
extern te_errno tapi_package_rpm_verify(tapi_job_factory_t *factory,
                                        const tapi_package_ctx *ctx,
                                        const char *name, te_vec *changed);

/**
 * Add every non-empty, non-blank line of @p text to @p vec as an owned
 * string, trimmed of trailing carriage returns.
 *
 * @param text          Text.
 * @param vec           Vector of @c char @c * (autoptr).
 */
extern void tapi_package_lines_to_vec(const char *text, te_vec *vec);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_PACKAGE_INTERNAL_H__ */
