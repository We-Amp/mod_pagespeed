# Pre-generated APR / apr-util configure headers

APR and apr-util normally produce a handful of platform headers at build time by
running their autoconf `./configure`. This repo vendors the C sources but builds
them with Bazel, which does not run `./configure`. Instead we check in the
`./configure` output per target platform and select the right tree with a
Bazel `select()`.

## Layout

```
third_party/apr/gen/arch/<os>/<cpu>/include/       apr.h, apr_private.h
third_party/aprutil/gen/arch/<os>/<cpu>/include/   apu.h, apu_want.h, apr_ldap.h,
                                                   private/apu_config.h,
                                                   private/apu_select_dbm.h
```

Trees currently present: `linux/x64`, `linux/arm64`, `mac/x64`.

Arch selection lives in:

- `third_party/apr/BUILD`, `third_party/aprutil/BUILD` — `apr.h`/`apu.h` filegroups
- `bazel/apr.bzl`, `bazel/aprutil.bzl` — `-I` include-path `copts`
- `pagespeed/apache/BUILD`, `test/pagespeed/apache/BUILD` — apache targets' `copts`
- `pagespeed.bazelrc` — `build:linux` injects the x64 gen `-I` globally (for
  targets that include APR headers without local copts); `build:ci-linux-arm64`
  adds the arm64 equivalents. In sandboxed builds only the select()-chosen tree
  exists, so the unused `-I` resolves nothing and is ignored.

All of the above key off `//bazel:linux_arm64` (os:linux + cpu:arm64); macOS is
matched separately, and x64 Linux is the `//conditions:default` branch, so the
x64 and macOS trees are selected byte-for-byte as before.

## Which macros are arch-dependent

`apr.h` and `apr_private.h` are the arch-varying files (pointer/word size,
endianness, integer-format strings, atomic-builtin availability). The apr-util
headers (`apu.h`, `apu_config.h`, ...) are feature/library-detection only and do
not vary by CPU on the same distro. On LP64 little-endian Linux the x64 and arm64
values agree for the ABI-critical macros (`APR_SIZEOF_VOIDP 8`,
`APR_IS_BIGENDIAN 0`, `WORDS_BIGENDIAN` undef, `SIZEOF_VOIDP/LONG/OFF_T/SIZE_T 8`,
`HAVE_ATOMIC_BUILTINS 1`).

## Regenerating (honest configure, no hand-editing)

The `linux/arm64` tree was produced by running the real APR / apr-util
`./configure` on **aarch64 Linux**, in the **same base image the arm64 CI lane
compiles in** (so the detected libc/feature set matches the runtime):

- Base image: `envoyproxy/envoy-build-ubuntu@sha256:1b3c82ca34c505c4951918b2e0a0c3db88cf266ebbf4196e4b0fba8fa137ada3`
  (Ubuntu 20.04, glibc 2.31, aarch64) — the `FROM` in `docker/Dockerfile`.
- Sources: the exact commits pinned in `bazel/repositories.bzl`
  (`APR_COMMIT` / `APRUTIL_COMMIT`).
- Flags: stock `./configure` (apr-util with `--with-apr=<apr>`). No extra flags —
  Bazel compiles a curated source subset and only consumes the headers listed
  above; the DBM/LDAP/DBD/crypto backends that configure flags would toggle are
  not built.

Reproduce with:

```
tools/gen-apr-arm64-headers.sh
```

(on an Apple-Silicon Mac or any host with arm64 Docker). It downloads the pinned
sources, runs `buildconf` + `configure` in the pinned image, and prints where it
wrote the headers.

## Notes / caveats

- The `linux/x64` tree is older-vintage than the currently pinned APR source and
  is hand-patched for a newer glibc (`HAVE_DECL_SYS_SIGLIST 0`, see the commit
  that added it). The `linux/arm64` tree is freshly generated from the pinned
  source, so it is cosmetically newer; the active ABI macros are identical to
  x64 (e.g. `APR_OFF_T_FMT` is `"ld"` on both — the `"lld"` in the file sits in
  a dead `DARWIN_10` branch). It carries `HAVE_DECL_SYS_SIGLIST 1`, which is
  correct on the arm64 lane's glibc 2.31 base.
- glibc stopped declaring `sys_siglist` in 2.32 and removed the compat symbol
  in 2.34. If the CI base image is bumped past glibc 2.31, regenerate this tree
  (or apply the same `HAVE_DECL_SYS_SIGLIST 0` edit) — the same forward-compat
  caveat that already applies to `linux/x64`.
