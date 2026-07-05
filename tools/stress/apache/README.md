# Apache ASan process-lifecycle stress rig

Runs the ASan-instrumented `libmod_pagespeed.so` under the canonical Apache
system-test config (`test/system/setup_apache_test.sh` — CoreFilters, IPRO, small
prefork MPM), driving `apache2 -k graceful` reloads and hard restarts under load,
then sweeps `error.log` (and cores) for AddressSanitizer reports. This is "the
Apache system-test rig under ASan" from the sanitizer-rig umbrella work. Companion to `../iis/` and
`../nginx/`; driven nightly by the Apache ASan lane. Grounded in the proven shutdown-UAF recipe
(`../cell_run.sh` + the multi-site sanitizer matrix).

## Key mechanics

- ASan is delivered by **LD_PRELOADing the clang runtime via `/etc/apache2/envvars`**
  (a markered, backed-up block) — `apache2ctl` sources it on start, so
  `setup_apache_test.sh start` brings up an ASan Apache with no wrapper.
  `ASAN_OPTIONS=detect_leaks=0` (intentional at-exit leaks — LSan stays on for
  unit tests), `abort_on_error=1`, `exitcode=66`.
- Setup **verifies** the runtime is mapped (`grep libclang_rt.asan /proc/<pid>/maps`)
  before proceeding — a module that silently loaded without ASan fails fast.
- **Gate:** `sweep_apache_asan_rig.sh` exits non-zero on the harness rc, any hard
  sanitizer/crash token in `error.log`, or a core.
- **Cleanup restores `/etc/apache2/envvars` verbatim** from the setup backup, so the
  next (non-ASan) Apache System Tests run is not silently ASan-preloaded.

## Scripts

| Script | Role |
|---|---|
| `setup_apache_asan_rig.sh` | Inject ASan envvars, `setup_apache_test.sh start` with the ASan module, verify ASan loaded, gen corpus, write reload/restart helpers |
| `run_apache_asan_rig.sh` | Health-check, drive `stress_shutdown.py --server apache` with graceful reload / hard restart chaos |
| `sweep_apache_asan_rig.sh` | Scan `error.log` tokens + cores — **exit 1 on any hit** |
| `cleanup_apache_asan_rig.sh` | Stop Apache, restore `/etc/apache2/envvars`, remove corpus (runs on failure too) |

Requires `apache2` + passwordless `sudo`. Env knobs: `MODULE_SO`, `DOCROOT`
(`/var/www/html`), `PORT` (80), `ERRLOG`, `RIG_DIR`, `LICENSE_TOKEN`,
`ASAN_RUNTIME`, `DURATION`, `CONCURRENCY`, `REPO_DIR`.

## Build the module

```sh
bazelisk build --config=clang-asan //:libmod_pagespeed.so
```
