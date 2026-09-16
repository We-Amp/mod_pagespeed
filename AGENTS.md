# AGENTS.md

`CLAUDE.md` is the canonical agent guide for this repo — read it first, along
with the per-subsystem `pagespeed/*/CLAUDE.md` files (apache, nginx, envoy, iis,
kernel, system) and `html/CLAUDE.md`. This file only restates the facts agents
most often miss; it does not duplicate `CLAUDE.md`.

## Must-not-miss facts

- **Default branch is `master`, not `main`.** Branch from and target `master`.
- **This build box holds many clones/worktrees.** Run `git rev-parse --show-toplevel`
  before editing; never edit sibling clones or vendored copies.
- **Two source trees.** Rewriter filters + `RewriteDriver`/`RewriteOptions` live
  under `net/instaweb/` (not `pagespeed/`). See "Two Source Trees" in `CLAUDE.md`.
- **Builds and tests run inside the Docker dev container.** The host toolchain is
  too old: `docker compose up -d && docker compose exec dev bash`.
- **Preferred Bazel config is `--config=clang-libstdcxx13`** (Clang + GCC 13
  libstdc++). C++ unit tests need the test_env quartet:
  `--test_env=REDIS_HOST=redis --test_env=REDIS_PORT=6379`
  `--test_env=MEMCACHED_HOST=memcached --test_env=MEMCACHED_PORT=11211`.
- **Match CI locally before pushing:** `tools/format.sh` (the single entry point —
  resolves clang-format **20.x** itself, so it matches CI even if your local
  clang-format is a newer major) and `tools/tidyup.sh` (clang-tidy-20).
  `tools/install-hooks.sh` also wires a `pre-push` gate. See "Linting &
  Formatting" in `CLAUDE.md`.
- **Two name-colliding factory files:** the live IIS factory is
  `pagespeed/iis/iis_rewrite_driver_factory.cpp`; `pagespeed/windows/iis_rewrite_driver_factory.cc`
  is build-inert (`tags = ["manual"]`). See "IIS Platform Internals" in `CLAUDE.md`.
- **Customer-facing 1.1 docs live in `pagespeed-optimizer/website/src/content/docs-1.1/`**,
  not in this repo's `docs/`.
- **A PR touching `pagespeed/`, `net/` or `install/` must carry a release note**
  — a blocking check (`release-note-guard` in the maintainer CI). Add an entry to `RELEASE_NOTES.md` (top,
  in-development section) *or* `CHANGELOG.md` (`## [Unreleased]`); either one is
  enough. Tests, `BUILD` files and `*.md` do not trigger it; `pagespeed/iis/` does.
  If nothing observable changed, waive it with a reason (≥12 chars) in a commit
  message or the PR body: `Release-Note: none - <why this is invisible to users>`.
  Check first: `bash tools/ci/check_release_note.sh --base origin/master --head HEAD`.
