# Code Coverage Reporting

**Status:** Active

## Context

The project has sanitizer configs (ASan, TSan) and Docker test scripts but no code coverage support. Adding coverage lets us see which lines in `lib/` and `src/` are exercised by the ~56 unit tests.

## Approach

Use Bazel's built-in `bazel coverage` command with LCOV output, plus `genhtml` for HTML reports. No experimental LLVM flags — just the standard gcov-compatible mode that clang supports natively.

## Changes

### 1. `.bazelrc` — add coverage defaults (after line 72)

```
# Coverage (applies automatically to `bazel coverage` subcommand)
coverage --combined_report=lcov
coverage --instrumentation_filter="//lib[/:],//src[/:]"
coverage:macos --test_env=COVERAGE_GCOV_PATH=/opt/homebrew/opt/llvm/bin/llvm-cov
```

- `combined_report=lcov` merges per-test LCOV into one file
- `instrumentation_filter` instruments only our code (not test/, third_party/, externals)
- macOS needs Homebrew's `llvm-cov` as gcov (Apple's toolchain gcov is incompatible)

### 2. `tools/coverage.sh` — native coverage script (~45 lines)

- Runs `bazel coverage //...` (or user-specified targets)
- Locates combined LCOV at `$(bazel info output_path)/_coverage/_coverage_report.dat`
- Runs `genhtml` to produce `coverage-report/index.html`
- Prints summary via `lcov --summary`
- Prerequisite: `brew install lcov` on macOS

### 3. `tools/docker-coverage.sh` — Docker coverage script (~45 lines)

- Follows `docker-test.sh` pattern (read-only mount, cache volume, platform detection)
- Excludes libaom-heavy tests by default (same as `docker-sanitizers.sh` — OOM risk)
- Mounts `coverage-report/` read-write to extract the HTML report from the container
- Runs `genhtml` inside the container

### 4. `docker/Dockerfile` — add `lcov` package

Add `lcov` to the `apt-get install` list (line 9). ~2MB addition.

### 5. `.gitignore` — add `/coverage-report/`

### 6. `CLAUDE.md` — add coverage section under Code Quality

```
### Code Coverage
./tools/coverage.sh                    # Native (requires: brew install lcov)
./tools/coverage.sh //lib/...          # Specific targets
./tools/docker-coverage.sh             # Docker
```

## Verification

1. Run `./tools/coverage.sh //lib/base/...` (small target set, fast)
2. Check `coverage-report/index.html` opens in browser with line-level annotations
3. Verify only `lib/` and `src/` files appear (no test/ or third_party/)
