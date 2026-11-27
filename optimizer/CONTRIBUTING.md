# Contributing to pagespeed-optimizer

Thanks for helping. This repository holds the mod_pagespeed 2 optimization
worker (`pagespeed-optimizer`), the nginx serving module shipped in the
container images and Helm chart, the management console, the ASP.NET Core
middleware and the modpagespeed.com website. The Apache, IIS and Envoy
modules, and the packaged nginx module (`nginx-module-pagespeed` from
apt/dnf and the `ngx_pagespeed` tarballs), live in
[We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed).

## Reporting bugs and requesting features

Open an issue at <https://github.com/We-Amp/pagespeed-optimizer/issues> using
the bug report or feature request form. Search existing issues first. A useful
bug report names:

- the version (`pagespeed-optimizer --version`, or the package version),
- the platform: distribution and architecture, web server and its version,
  and how you installed (signed apt/dnf repository, container image, Helm
  chart, NuGet package, or built from source),
- the smallest configuration excerpt that reproduces it,
- the `X-PageSpeed` response header of an affected request
  (`curl -s -o /dev/null -D - <url> | grep -i x-pagespeed`; use GET, not
  `curl -I`), and any relevant log lines.

**Security vulnerabilities:** do not open a public issue. Follow
[SECURITY.md](SECURITY.md). For commercial support see [SUPPORT.md](SUPPORT.md).

## Building and testing

Prerequisites, as in the README's [Development](README.md#development)
section: [Bazelisk](https://github.com/bazelbuild/bazelisk) (the Bazel
version is pinned in `.bazelversion`), clang-20 with libc++-20 from
[apt.llvm.org](https://apt.llvm.org/) (the distribution compiler does not
build this tree), Docker for the sanitizer and end-to-end suites, and
[pre-commit](https://pre-commit.com/).

```bash
export CC=clang-20 CXX=clang++-20

bazel build --config=libc++ //...   # everything except the nginx module
bazel test  --config=libc++ //...   # unit tests

# nginx module (needs nginx source headers)
NGINX_PATH=/path/to/nginx bazel build --config=libc++ //src/nginx:ngx_pagespeed_module.so

./tools/docker-sanitizers.sh        # ASan + UBSan + LeakSan + TSan
./tools/e2e/run_e2e.sh              # full stack in Docker Compose
```

Website changes (`website/`) are ordinary pull requests too; see
[RELEASING.md](RELEASING.md#the-website-tree) for the checks CI runs on them.

## Code style

- C++23, Google style, 80-column limit.
- Install the hooks once per clone: `pre-commit install`. The commit hook
  runs clang-format, buildifier, ruff and the license-header check; the push
  hook re-checks formatting with the pinned clang-format 20.x
  (`tools/format.sh`).

## Pull requests

- Branch from `main` and keep each pull request to one change.
- Add or update tests for behaviour changes; `bazel test --config=libc++ //...`
  should pass locally.
- **Release notes are required.** A pull request that touches `src/` or `lib/`
  (outside tests and build files) must add a changelog fragment,
  `changelog.d/<branch-slug>.<category>.md` (see
  [changelog.d/README.md](changelog.d/README.md)), instead of editing
  [CHANGELOG.md](CHANGELOG.md); write it for a user deciding whether to
  upgrade. If the change is invisible to users, put
  `Release-Note: none — <why>` in a commit message or the pull request
  description (editing the description does not re-run the check; re-run
  the job). Check it locally with
  `bash tools/ci/check_release_note.sh --base origin/main --head HEAD`.
  Details in [RELEASING.md](RELEASING.md).
- Describe the problem, the change and how you verified it.
- Pull requests from forks are welcome. They merge through the merge queue
  like any other, with CI running on GitHub-hosted runners (slower than the
  maintainers' own runs). Contributors cannot add their own pull requests to
  the merge queue. A maintainer reads the full diff of a pull request from a
  fork, including any `.github/` or `tools/ci/` changes, before adding it to
  the queue; fork pull requests that change those paths are re-landed from a
  branch instead.

## Licensing

The project is licensed under the [Apache License 2.0](LICENSE).

## Release history

There is no published roadmap. Changes are recorded in
[CHANGELOG.md](CHANGELOG.md) and the
[release notes](https://modpagespeed.com/docs/release-notes/) on
modpagespeed.com.
