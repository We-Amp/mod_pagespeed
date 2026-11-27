# lib/pagespeed -- C API (libpagespeed.so / pagespeed.dll)

## Purpose

Pure C API wrapper exposing mod_pagespeed functionality as a shared library for
cross-language integration. Primary consumer: ASP.NET Core middleware in
`samples/aspnetcore/` which calls this API via P/Invoke (DllImport).

## API Design

- **Opaque handle types**: `ps_cache_t*`, `ps_read_result_t*`, `ps_write_handle_t*`,
  `ps_html_result_t*`, `ps_scan_result_t*`, etc. Callers never see internal C++ state.
- **Error codes**: `ps_error_t` enum (PS_OK, PS_ERR_NOT_FOUND, PS_ERR_IO, etc.) with
  `ps_error_name()`, `ps_strerror()`, and thread-local `ps_last_error_message()`.
- **Config structs**: Versioned via `struct_size` field for ABI-compatible extension.
  Always initialize with `ps_*_config_init()` before use.
- **PS_NODISCARD**: All error-returning functions are marked warn-unused-result.

## Key Function Groups

| Group | Functions | Purpose |
|-------|-----------|---------|
| **Version** | `ps_version_major/minor/patch`, `ps_git_commit` | Runtime version queries |
| **Classification** | `ps_classify`, `ps_classify_content_type`, `ps_mask_set_viewport_from_width` | Request capability bitmask and content type detection |
| **Cache lifecycle** | `ps_cache_open`, `ps_cache_close`, `ps_cache_config_init` | Open/close Cyclone cache volume |
| **Cache reads** | `ps_cache_read_best`, `ps_cache_read_alternate`, `ps_cache_read_early_hints` | Variant-aware cache lookups |
| **Cache writes** | `ps_cache_write_begin`, `ps_write_data`, `ps_write_close`, `ps_write_abort` | Streaming cache writes |
| **Cache management** | `ps_cache_remove`, `ps_cache_list_alternates`, `ps_cache_stats` | Cache inspection and eviction |
| **HTML processing** | `ps_html_process`, `ps_html_scan`, `ps_html_transform_*` | Full HTML optimization pipeline |
| **HTML scanning** | `ps_html_scan`, `ps_scan_get_*`, `ps_scan_result_destroy` | Scan HTML for resources and metadata |
| **HTML transform** | `ps_html_transform_*` | Transform HTML with PageSpeed optimizations |
| **Critical CSS** | `ps_css_extract_critical`, `ps_critical_css_output` | Above-the-fold CSS extraction |
| **CSS utilities** | `ps_css_validate`, `ps_css_minify`, `ps_css_flatten_imports` | CSS processing tools |
| **Alternate scoring** | `ps_score_alternate` | Score alternate variants |
| **Hostname normalization** | `ps_normalize_hostname` | Normalize hostnames for cache keys |
| **Memory** | `ps_free` | Free allocations returned by the API |
| **Worker notification** | `ps_notify_worker`, `ps_notify_worker_ex`, `ps_notify_params_init_sized` | Send optimization requests to worker via Unix socket; the `_ex` form also carries the agent-request bit and the per-request options context |
| **HTML cacheability report** | `ps_html_store_classify`, `ps_html_decision_format`, `ps_html_store_reason_name` | Pure would-store verdicts for `text/html` (PS_API 1.12) and the one-line decision record with a closed key set and a per-key value grammar. Scalars and parallel `names`/`values` arrays only, so no struct crosses the ABI |
| **Options context** | `ps_option_context_signature` | Signature over a canonical options-context payload. One implementation, exported, because every consumer must produce identical signatures for identical payloads forever |

## Lifetime Management

- Every `ps_*_open` / `ps_*_create` / `ps_*_begin` has a matching `ps_*_close` / `ps_*_free` / `ps_*_abort`.
- Thread safety: error messages are thread-local. Cache handles are thread-safe (internal mutex).
  Individual read results and write handles must not be shared across threads.
- All `extern "C"` functions wrap their bodies in `try/catch(...)` to prevent C++ exceptions
  from crossing the ABI boundary.

## Build

```bash
bazel build //lib/pagespeed:pagespeed_api       # Static library (for linking into other targets)
bazel build //lib/pagespeed:libpagespeed.so      # Shared library (tagged manual)
```

The shared library build uses platform-specific symbol export control:
- Linux: `symbols.lds` (version script)
- macOS: `symbols.exp` (exported symbols list)
- Windows: `symbols.def` (module definition file, `__declspec(dllexport)`)

All three files must be kept in sync when adding public API functions.

## Test

```bash
bazel test //lib/pagespeed:pagespeed_test        # C API unit tests (size=large for ASan)
```

Tests cover version constants, error codes, content type classification, capability mask
encoding, cache open/read/write/close lifecycle, HTML processing, CSS operations,
and cross-function integration scenarios.

## ABI Considerations

- `PS_EXPORT` macro handles `__declspec(dllexport/dllimport)` on Windows vs default visibility
- `PS_BUILDING_SHARED` define is set automatically for Windows shared library builds
- Config structs use `struct_size` for forward/backward compatibility -- new fields are
  appended and defaulted by `*_config_init()`, so older consumers work with newer libraries
- Enum values include `*_FORCE_INT = 0x7FFFFFFF` sentinels to force 32-bit representation

## Relationship to samples/aspnetcore/

The `samples/aspnetcore/` directory contains the ASP.NET Core middleware that consumes
this C API. The middleware loads `libpagespeed.so` (Linux) or `pagespeed.dll` (Windows)
at runtime via P/Invoke. The shared library is built by Bazel and copied into the
NuGet package or Docker image during the multi-stage build process.
