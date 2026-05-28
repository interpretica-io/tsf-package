/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Package managers of a Test Agent
 *
 * @defgroup tapi_package Package managers of a Test Agent (tapi_package)
 * @{
 *
 * Installing, removing and asking about software on an agent through
 * whichever package manager it has, behind one interface.
 *
 * A suite needs this two ways round. As a **means**: a test that wants
 * a tool on the agent - @c tcpdump for a capture, a driver's user-space
 * package, the dependencies of the thing under test - installs it rather
 * than assuming it is there. As an **end**: on a device under test the
 * package manager is itself part of the product, and a test asks it what
 * is installed, checks a package's files are the ones it shipped, or
 * makes installing a bad package the thing under test.
 *
 * The managers this drives:
 *
 * | Manager | Tool | Where |
 * |---|---|---|
 * | #TAPI_PACKAGE_APT | @c apt-get, @c dpkg-query | Debian, Ubuntu |
 * | #TAPI_PACKAGE_DNF | @c dnf, @c rpm | Fedora, RHEL |
 * | #TAPI_PACKAGE_ZYPPER | @c zypper, @c rpm | SUSE |
 * | #TAPI_PACKAGE_PACMAN | @c pacman | Arch |
 * | #TAPI_PACKAGE_APK | @c apk | Alpine |
 * | #TAPI_PACKAGE_OPKG | @c opkg | OpenWrt and other embedded |
 * | #TAPI_PACKAGE_BREW | @c brew | macOS, Linuxbrew |
 *
 * tapi_package_detect() finds which one the agent has. From there the
 * operations - tapi_package_install(), tapi_package_remove(),
 * tapi_package_query(), tapi_package_list(), tapi_package_files(),
 * tapi_package_verify() - are the same call whichever manager answers;
 * the differences in command line, output format and what each supports
 * are behind the interface.
 *
 * @code
 * tapi_package_ctx ctx;
 * tapi_package_info pkg;
 *
 * CHECK_RC(tapi_package_ctx_init(factory, &ctx));    // detect + defaults
 * CHECK_RC(tapi_package_ensure(factory, &ctx, "tcpdump"));
 * CHECK_RC(tapi_package_query(factory, &ctx, "tcpdump", &pkg));
 * RING("tcpdump %s is installed", pkg.version);
 * tapi_package_info_free(&pkg);
 * tapi_package_ctx_fini(&ctx);
 * @endcode
 *
 * @section tapi_package_state Restoring the agent
 *
 * A test that installs something should leave the agent as it found it.
 * tapi_package_installed() before and tapi_package_remove() of what was
 * not there after is the manual way; tapi_package_ensure() /
 * tapi_package_ensure_absent() record what they changed in the context,
 * and tapi_package_ctx_rollback() undoes exactly that, which is what a
 * cleanup section wants.
 *
 * @note Everything here needs an RPC job factory, for the reason given
 *       in tsf-devtool: reading what a tool printed needs output
 *       channels, which only that factory implements.
 *
 * @note Installing reaches the network unless the agent is configured
 *       with a local mirror; a test that must not depend on the network
 *       either installs from a file (tapi_package_install_file()) or
 *       points the manager at a mirror the lab controls.
 */

#ifndef __TSF_TAPI_PACKAGE_H__
#define __TSF_TAPI_PACKAGE_H__

#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default timeout of an install or remove, ms. The network is slow. */
#define TAPI_PACKAGE_TIMEOUT_MS         600000

/** Timeout of a query, ms. */
#define TAPI_PACKAGE_QUERY_TIMEOUT_MS   60000

/** A package manager. */
typedef enum tapi_package_manager {
    /** None was found, or none was set. */
    TAPI_PACKAGE_NONE = 0,
    /** Debian/Ubuntu: @c apt-get and @c dpkg. */
    TAPI_PACKAGE_APT,
    /** Fedora/RHEL: @c dnf and @c rpm. */
    TAPI_PACKAGE_DNF,
    /** SUSE: @c zypper and @c rpm. */
    TAPI_PACKAGE_ZYPPER,
    /** Arch: @c pacman. */
    TAPI_PACKAGE_PACMAN,
    /** Alpine: @c apk. */
    TAPI_PACKAGE_APK,
    /** OpenWrt and other embedded: @c opkg. */
    TAPI_PACKAGE_OPKG,
    /** macOS and Linuxbrew: @c brew. */
    TAPI_PACKAGE_BREW,
} tapi_package_manager;

/**
 * Spell out a manager, as used in check identifiers and logs.
 *
 * @param manager       Manager.
 *
 * @return A static string, never @c NULL.
 */
extern const char *tapi_package_manager2str(tapi_package_manager manager);

/** How a package manager operation is run. */
typedef struct tapi_package_ctx {
    /** Manager to use; set by tapi_package_ctx_init() or by hand. */
    tapi_package_manager manager;
    /** Run the tool through @c sudo. */
    bool sudo;
    /** Answer yes to prompts (the default; clear only for a dry run). */
    bool assume_yes;
    /** Timeout of an install or remove, ms; 0 for the default. */
    int timeout_ms;
    /**
     * Do not touch the network: refuse an operation that would fetch,
     * and pass the manager's offline flag where it has one. An install
     * of an already cached or local package still works.
     */
    bool offline;
    /**
     * Packages tapi_package_ensure() installed and
     * tapi_package_ensure_absent() removed, for
     * tapi_package_ctx_rollback(). Managed by the context; do not touch.
     */
    te_vec installed_by_us;
    /** Packages ensure_absent() removed and should put back. */
    te_vec removed_by_us;
} tapi_package_ctx;

/**
 * Detect the manager, set the defaults (assume yes, sudo off, default
 * timeout) and prepare the rollback records.
 *
 * @param[in]  factory  Job factory.
 * @param[out] ctx      Context; release it with tapi_package_ctx_fini().
 *
 * @return Status code.
 * @retval TE_ENOENT    No known manager on the agent.
 */
extern te_errno tapi_package_ctx_init(tapi_job_factory_t *factory,
                                      tapi_package_ctx *ctx);

/**
 * Roll back what tapi_package_ensure() and tapi_package_ensure_absent()
 * changed: remove what they installed, reinstall what they removed.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 *
 * @return Status code.
 */
extern te_errno tapi_package_ctx_rollback(tapi_job_factory_t *factory,
                                          tapi_package_ctx *ctx);

/**
 * Release a context. Does not roll anything back; call
 * tapi_package_ctx_rollback() first if that is wanted.
 *
 * @param ctx           Context.
 */
extern void tapi_package_ctx_fini(tapi_package_ctx *ctx);

/**
 * Detect which manager the agent has, trying the tools in a fixed order.
 *
 * @param[in]  factory  Job factory.
 * @param[out] manager  Where to save what was found.
 *
 * @return Status code.
 */
extern te_errno tapi_package_detect(tapi_job_factory_t *factory,
                                    tapi_package_manager *manager);

/** What one install or remove did. */
typedef struct tapi_package_result {
    /** @c true if the tool exited successfully. */
    bool ok;
    /** Exit status of the tool, or @c -1 if it did not exit normally. */
    int status;
    /** Everything the tool printed, kept for the log and for verdicts. */
    te_string output;
} tapi_package_result;

/**
 * Prepare an empty result.
 *
 * @param result        Result.
 */
extern void tapi_package_result_init(tapi_package_result *result);

/**
 * Release a result.
 *
 * @param result        Result.
 */
extern void tapi_package_result_free(tapi_package_result *result);

/** What is known about one package. */
typedef struct tapi_package_info {
    /** Name. */
    char *name;
    /** Installed version, or @c NULL if it is not installed. */
    char *version;
    /** Architecture, or @c NULL if the manager does not report it. */
    char *arch;
    /** @c true if the package is installed. */
    bool installed;
} tapi_package_info;

/**
 * Release a package info.
 *
 * @param pkg           Package info.
 */
extern void tapi_package_info_free(tapi_package_info *pkg);

/**
 * Bring the package index up to date (@c apt-get @c update and the like).
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 *
 * @return Status code.
 */
extern te_errno tapi_package_update(tapi_job_factory_t *factory,
                                    const tapi_package_ctx *ctx);

/**
 * Install or upgrade packages.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  names    Package names.
 * @param[in]  n_names  Number of @p names.
 * @param[out] result   Result; release with tapi_package_result_free().
 *
 * @return Status code.
 */
extern te_errno tapi_package_install(tapi_job_factory_t *factory,
                                     const tapi_package_ctx *ctx,
                                     const char **names, size_t n_names,
                                     tapi_package_result *result);

/**
 * Install a package from a file on the agent (a @c .deb, @c .rpm,
 * @c .apk or @c .ipk), rather than from a repository. A test that must
 * not reach the network installs this way.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  path     Path of the package file on the agent.
 * @param[out] result   Result; release with tapi_package_result_free().
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP The manager does not install from a file.
 */
extern te_errno tapi_package_install_file(tapi_job_factory_t *factory,
                                          const tapi_package_ctx *ctx,
                                          const char *path,
                                          tapi_package_result *result);

/**
 * Remove packages.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  names    Package names.
 * @param[in]  n_names  Number of @p names.
 * @param[in]  purge    Remove configuration too, where the manager can.
 * @param[out] result   Result; release with tapi_package_result_free().
 *
 * @return Status code.
 */
extern te_errno tapi_package_remove(tapi_job_factory_t *factory,
                                    const tapi_package_ctx *ctx,
                                    const char **names, size_t n_names,
                                    bool purge, tapi_package_result *result);

/**
 * Ask whether a package is installed.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  name     Package name.
 * @param[out] installed Where to save the answer.
 *
 * @return Status code.
 */
extern te_errno tapi_package_installed(tapi_job_factory_t *factory,
                                       const tapi_package_ctx *ctx,
                                       const char *name, bool *installed);

/**
 * Query one package: whether it is installed and, if so, its version and
 * architecture.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  name     Package name.
 * @param[out] pkg      Package info; release with tapi_package_info_free().
 *
 * @return Status code.
 */
extern te_errno tapi_package_query(tapi_job_factory_t *factory,
                                   const tapi_package_ctx *ctx,
                                   const char *name, tapi_package_info *pkg);

/**
 * List every installed package.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[out] packages Vector of #tapi_package_info to append to,
 *                      initialized with @c TE_VEC_INIT(tapi_package_info);
 *                      release it with tapi_package_list_free().
 *
 * @return Status code.
 */
extern te_errno tapi_package_list(tapi_job_factory_t *factory,
                                  const tapi_package_ctx *ctx,
                                  te_vec *packages);

/**
 * Release a package list.
 *
 * @param packages      Vector of #tapi_package_info.
 */
extern void tapi_package_list_free(te_vec *packages);

/**
 * List the files a package owns.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  name     Package name.
 * @param[out] paths    Vector of @c char @c * to append to, initialized
 *                      with @c TE_VEC_INIT_AUTOPTR(char *).
 *
 * @return Status code.
 */
extern te_errno tapi_package_files(tapi_job_factory_t *factory,
                                   const tapi_package_ctx *ctx,
                                   const char *name, te_vec *paths);

/**
 * Verify a package's files against the manager's own database: what has
 * been changed, replaced or removed since it was installed.
 *
 * @param[in]  factory  Job factory.
 * @param[in]  ctx      Context.
 * @param[in]  name     Package name.
 * @param[out] changed  Vector of @c char @c * (autoptr) to append the
 *                      paths that no longer match to.
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP The manager cannot verify (e.g. @c opkg).
 */
extern te_errno tapi_package_verify(tapi_job_factory_t *factory,
                                    const tapi_package_ctx *ctx,
                                    const char *name, te_vec *changed);

/**
 * Make sure a package is installed, installing it if it is not, and
 * record that this test installed it so tapi_package_ctx_rollback() can
 * remove it again. A package that was already there is left, and not
 * recorded.
 *
 * @param[in]  factory  Job factory.
 * @param[in,out] ctx   Context.
 * @param[in]  name     Package name.
 *
 * @return Status code.
 */
extern te_errno tapi_package_ensure(tapi_job_factory_t *factory,
                                    tapi_package_ctx *ctx, const char *name);

/**
 * Make sure a package is not installed, removing it if it is, and record
 * that this test removed it so tapi_package_ctx_rollback() can put it
 * back.
 *
 * @param[in]  factory  Job factory.
 * @param[in,out] ctx   Context.
 * @param[in]  name     Package name.
 *
 * @return Status code.
 */
extern te_errno tapi_package_ensure_absent(tapi_job_factory_t *factory,
                                           tapi_package_ctx *ctx,
                                           const char *name);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_PACKAGE_H__ */

/**@} <!-- END tapi_package --> */
