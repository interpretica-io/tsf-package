# tsf-package

The package manager of a Test Agent, behind one interface across the
managers agents actually run — packaged as an external Test Environment
(TE) repository and consumed with the `TE_EXT_REPO` builder directive.

Library:

- `tapi_package` — engine-side TAPIs, built as a shared library. One set
  of calls — detect, update, install, remove, query, list, files, verify,
  ensure/rollback — over seven managers:

  | Manager | Tool | Where |
  |---|---|---|
  | apt | `apt-get`, `dpkg-query`, `dpkg` | Debian, Ubuntu |
  | dnf | `dnf`, `rpm` | Fedora, RHEL |
  | zypper | `zypper`, `rpm` | SUSE |
  | pacman | `pacman` | Arch |
  | apk | `apk` | Alpine |
  | opkg | `opkg` | OpenWrt and other embedded |
  | brew | `brew` | macOS, Linuxbrew |

It builds on
[tsf-devtool](https://github.com/interpretica-io/tsf-devtool), for
running a tool on an agent and capturing what it printed.

## Usage

Declare the repositories in an external libraries catalog (e.g.
`conf/external.yml` in the test suite) and pass it to
`dispatcher.sh --external=external.yml`:

```yaml
repositories:
  - name: tsf_devtool
    url: https://github.com/interpretica-io/tsf-devtool.git
    ref: <tag>
    libs:
      - tapi_devtool
  - name: tsf_package
    url: https://github.com/interpretica-io/tsf-package.git
    ref: <tag>
    libs:
      - tapi_package
```

Bind them to the engine platform in `builder.conf`:

```
TE_EXT_REPO_USE([tsf_devtool], [], [tapi_devtool])
TE_EXT_REPO_USE([tsf_package], [], [tapi_package])
```

and add `tapi_package` to the `te_libs` list of the suite's `meson.build`.

Requires TE with `TE_EXT_REPO` support, and an **RPC** job factory
(`ta_rpcprovider` on the agent): everything here reads what a tool
printed, which needs output channels, and only that factory has them.

## Why

A suite needs this two ways round. As a **means**: a test that wants a
tool on the agent — `tcpdump` for a capture, the user-space package of a
driver, the dependencies of the thing under test — installs it rather
than assuming it is there, and puts the agent back afterwards. As an
**end**: on a device under test the package manager is part of the
product, and a test asks it what is installed, checks a package's files
are the ones it shipped, or makes installing a bad package the thing
under test.

The point is that the call is the same whichever manager answers; the
differences in command line, output format and what each manager can do
are behind the interface.

## The shape

```c
tapi_package_ctx ctx;
tapi_package_info pkg;

CHECK_RC(tapi_package_ctx_init(factory, &ctx));   /* detect + defaults */
CHECK_RC(tapi_package_ensure(factory, &ctx, "tcpdump"));
CHECK_RC(tapi_package_query(factory, &ctx, "tcpdump", &pkg));
RING("tcpdump %s is installed", pkg.version);
tapi_package_info_free(&pkg);

... the test ...

CHECK_RC(tapi_package_ctx_rollback(factory, &ctx));  /* undo our installs */
tapi_package_ctx_fini(&ctx);
```

`tapi_package_ctx_init()` detects the manager (in a fixed order) and sets
the defaults: assume yes, no sudo, the default timeout.

## Restoring the agent

A test that installs something should leave the agent as it found it.
`tapi_package_ensure()` installs a package only if it is missing and
records that it did; `tapi_package_ensure_absent()` removes one that is
present and records that; `tapi_package_ctx_rollback()` undoes exactly
those — removing what was installed, reinstalling what was removed —
which is what a cleanup section wants. A package that was already in the
state asked for is left alone and not recorded.

## Querying

- `tapi_package_installed()` — is it there?
- `tapi_package_query()` — installed, and if so version and architecture;
- `tapi_package_list()` — every installed package;
- `tapi_package_files()` — the files a package owns;
- `tapi_package_verify()` — the files that no longer match the manager's
  own database (changed, replaced, removed). Supported on apt (`dpkg -V`),
  dnf and zypper (`rpm -V`) and pacman (`pacman -Qkk`); apk, opkg and
  brew have no per-file verify and return `TE_EOPNOTSUPP`.

## Off the network

Installing reaches the network unless the agent has a local mirror. A
test that must not depend on it either installs from a file with
`tapi_package_install_file()` (a `.deb`, `.rpm`, `.apk` or `.ipk` already
on the agent), or sets `ctx.offline`, which passes each manager its
offline flag and skips `update`.

## Scope

Installing and removing software changes the agent for every later test
on it. `ensure()`/`rollback()` are there so a test can make its change
and take it back; a suite that installs without rolling back is changing
the lab, which is the suite's call to make on purpose, not by accident.
