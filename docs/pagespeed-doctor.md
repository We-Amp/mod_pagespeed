# pagespeed-doctor

`pagespeed-doctor` answers one question: does mod_pagespeed work on this
server, and if not, why? It is installed by the Apache (`mod-pagespeed`) and
nginx (`nginx-module-pagespeed`) packages and runs in a few seconds:

```bash
sudo pagespeed-doctor
```

```
pagespeed-doctor 2.9.0 -- nginx 1.24.0 (ubuntu-24.04), nginx-module-pagespeed 2.9.0-r1~noble

  PASS  D02 Module loaded     ngx_pagespeed_module.so loaded once via /etc/nginx/modules-enabled/50-mod-pagespeed.conf:3
  FAIL  D03 Config test       config test fails: unknown pagespeed option FileCachePathh; did you mean FileCachePath? (/etc/nginx/conf.d/pagespeed.conf:5)
        fix: Correct the option name to FileCachePath, then run the config test again.
        see: sed -n '5p' /etc/nginx/conf.d/pagespeed.conf
        docs: https://modpagespeed.com/docs/configuration/
  SKIP  D04 Module answering  skipped: depends on D03
  ...

Verdict: NOT WORKING -- 1 failure(s), 0 warning(s). Fix D03 first.
```

Checks run in dependency order, and a check whose prerequisite failed is
skipped, so the first `FAIL` is the one to fix. Fix it, run the doctor again,
repeat.

## Checks

| ID | Check | Fails or warns when |
|----|-------|---------------------|
| D02 | Module loaded | the module is not loaded, is loaded twice (for example an old `load_module` line next to the package's `modules-enabled/50-mod-pagespeed.conf`), its file is missing, or it was built for another nginx version |
| D03 | Config test | `nginx -T` / `apachectl -t` fails. A pagespeed option in the failing line is classified: removed, misspelled (with the closest valid name), set in the wrong block (with where it belongs), or given the wrong arguments. Any other error is reported by class and file:line only (`evidence[].error_class`), never by its text. When the test passes, deprecated options and filters still in the configuration are listed once each (warning) |
| D04 | Module answering | the test page has no `X-Page-Speed` (nginx) or `X-Mod-Pagespeed` (Apache) header; the likely reason follows: `pagespeed off`/`standby`, no `pagespeed on`, not an HTML page, or the Apache output filter missing |
| D05 | Optimizing | the page has no optimized (`.pagespeed.`) URLs compared with its `?PageSpeed=off` copy. The statistics name the cause when there is one (resources with `Cache-Control: private/no-store`, unauthorized domains, fetch failures); otherwise it is a warning, because optimization is progressive: request the page again and re-run |
| D11 | Cache directory | a `FileCachePath` (or the optimizer volume directory) is not writable by the web server user, holds root-owned files, or sits on a file system over 90% full |
| D12 | Optimizer worker | `DaemonSocketPath` is set but the worker is not running, the socket is missing or not writable for the web server user, that user is not in the `pagespeed` group, or the health socket answers `DEGRADED` |

## Options

| Option | Meaning |
|--------|---------|
| `--json` | print one JSON document (below) and nothing else on stdout |
| `--server nginx\|apache` | pick the server when both are installed |
| `--host NAME` | send `Host: NAME` with the test request (the vhost to test) |
| `--path /page` | the page to test (default `/`) |
| `--port N` | the local port (default 80) |
| `--url URL` | test this URL instead of `http://127.0.0.1/`; statistics are read only from localhost |
| `--no-configtest` | do not run the config test (D03 is skipped) |
| `--config FILE` | the server's main configuration file when it is not the default (`nginx -c FILE`, `httpd -f FILE`) |
| `--allow-cache-writes` | run the config test even where it cannot be isolated from the cache directories (see below); it may then leave root-owned cache files, which D11 reports |
| `--verbose` | show file:line evidence for passing checks too |

Exit codes: `0` all checks pass, `1` warnings only, `2` at least one failure,
`3` the doctor could not run (bad option, no supported server).

A skipped check normally does not change the verdict: it simply does not
apply (D12 when no worker is configured). D03 is the exception. When the
config test did not run (it could not be isolated, `--no-configtest`, or no
root), the doctor cannot vouch for the configuration, so the verdict is at
least `working_with_warnings` (exit 1), and the summary ends with
`config test skipped: <reason>` (`summary.notes` in the JSON).

## JSON

`--json` prints a document with `"schema": "pagespeed-doctor/1"`. Check ids,
their order and the `code` values are stable within a schema version, so
scripts and coding agents should branch on `code`, not on `message`.

```json
{
  "schema": "pagespeed-doctor/1",
  "doctor_version": "2.9.0",
  "host": {"os": "ubuntu-24.04", "arch": "x86_64", "server": "nginx", "server_version": "1.24.0", "root": true},
  "verdict": "not_working",
  "exit_code": 2,
  "summary": {"pass": 1, "warn": 0, "fail": 1, "skip": 4},
  "checks": [
    {"id": "D03", "title": "Config test", "status": "fail", "signature": "S1",
     "code": "directive_typo",
     "message": "config test fails: unknown pagespeed option FileCachePathh; did you mean FileCachePath? (/etc/nginx/conf.d/pagespeed.conf:5)",
     "evidence": [{"file": "/etc/nginx/conf.d/pagespeed.conf", "line": 5, "directive": "FileCachePathh"}],
     "fix": {"hint": "Correct the option name to FileCachePath, then run the config test again.",
             "commands": [{"command": "sed -n '5p' /etc/nginx/conf.d/pagespeed.conf", "mutates": false}],
             "doc": "https://modpagespeed.com/docs/configuration/"},
     "depends_on": ["D02"], "duration_ms": 180}
  ]
}
```

`verdict` is `working`, `working_with_warnings` or `not_working` and matches
`exit_code`; `summary.notes` explains a verdict raised by a skipped config
test. A skipped check carries `"reason"`. `fix.commands` are
suggestions; the doctor never runs them, and a command that changes the
system is marked `"mutates": true`.

Codes: `module_not_loaded`, `module_loaded_twice`, `module_file_missing`,
`module_version_mismatch` (D02); `directive_removed`,
`directive_typo`, `directive_unknown`, `directive_scope`,
`directive_arguments`, `directive_value_invalid`, `config_test_failed`,
`directive_deprecated`, and `configtest_not_isolated` on a skipped D03; `server_not_reachable`, `header_missing`,
`module_off`, `module_standby`, `module_not_enabled`, `not_html`,
`output_filter_missing` (D04); `nothing_to_optimize`, `not_optimized_yet`,
`cache_control_not_rewritable`, `domain_not_authorized`,
`resource_fetch_failures` (D05); `cache_dir_missing`,
`cache_dir_not_writable`, `root_owned_cache_files`, `cache_disk_full`,
`configtest_unshielded` (D11);
`worker_not_running`, `worker_unreachable`, `user_not_in_group`,
`worker_socket_denied`, `worker_health_missing`, `worker_degraded`,
`worker_unhealthy`, `worker_health_unknown` (D12).

## What it does and does not do

- **Read-only.** It never edits configuration, reloads a service, purges a
  cache or changes ownership. The only files it writes are in its own
  temporary directory, removed on exit.
- **No secrets in the output.** Configuration is read in memory. The output
  names file:line and the option, and shows a value only for non-secret
  options (paths, on/off, filter names); anything else is `***`. Raw config
  test output is never printed.
- **Local only.** It talks to `127.0.0.1` (or the `--url` you give), local
  Unix sockets and local files, always without a proxy. No DNS lookups, no
  update checks, no telemetry.
- **The config test leaves the cache alone, or does not run.** Run as root,
  a config test can leave a root-owned cache file the web server user cannot
  replace (We-Amp/mod_pagespeed#780). The doctor first reads the
  configuration files itself, running nothing: the main file (`--config`,
  else the server's default) and the files it includes, recursively, with
  globs expanded, Apache directory includes followed into subdirectories,
  and relative paths resolved against nginx's configuration directory or
  Apache's `ServerRoot`. It collects each `FileCachePath` and
  `DaemonVolumePath`, nested ones included, plus the packaged default when
  no cache path is configured. It then runs the test in a private mount
  namespace with an empty scratch directory mounted over each cache
  directory, so the real cache is not touched. Every one of those
  directories must already exist: shielding a missing one would mean
  covering its parent, and with it the logs, certificates and mounts there.
  After the test it compares the cache directories of the effective
  configuration (`nginx -T`, or the file list of Apache's `DUMP_INCLUDES`)
  with what it shielded; a miss is reported by D11
  (`configtest_unshielded`), and `config.shield_verified` says whether the
  comparison could be made.
- **Fail closed.** The test is not run, and D03 is skipped with code
  `configtest_not_isolated`, when no private mount namespace is available
  (most containers), a mount fails, a cache directory does not exist yet
  (the hint says which one to create, owned by the web server user), an
  include path uses a `${VARIABLE}` the doctor cannot resolve, or pagespeed
  is configured but no `FileCachePath` was found. `--allow-cache-writes`
  runs it anyway; the doctor then lists root-owned cache files before and
  after, and D11 reports any the test created, with the `chown` that fixes
  them. The test runs without a shield only when the module is not loaded at
  all. Printed commands quote every path taken from the configuration.
- **Root gives the full result.** Without root the config test is skipped and
  some ownership checks are partial; the verdict says so.

## Known limits

- The static read follows `include` (nginx) and `Include` /
  `IncludeOptional` (Apache). It does not evaluate Apache `<IfModule>`,
  `<IfDefine>` or `Define`: it reads both branches, which can only add cache
  directories to the shield. `${VARIABLE}` in an Apache include path is
  resolved from the environment the doctor runs in; anything else makes the
  config test fail closed.
- Apache's `ServerRoot` is taken from the first `ServerRoot` directive in
  the main file, else from the server's compiled-in root.
- A cache directory that does not exist yet is never shielded by covering
  its parent: the test fails closed until the directory exists (it is
  created at the first server start, or by hand). A cache path containing a
  newline cannot be shielded either.
- An Apache build without `DUMP_INCLUDES`, or a config test that fails
  before nginx prints its dump, leaves the shield unverified; D11 says so.
- nginx started with `-p` (a prefix other than the compiled-in one) is not
  detected; pass the main file with `--config`.

Requirements: POSIX `sh`, `awk`, `sed`, `grep`, `find` and `curl`. `socat` or
`python3` is used for the worker's health socket when present.

## Packaging

The script lives in the module repo at `install/common/pagespeed-doctor`.
Each package installs it at `/usr/share/<package>/pagespeed-doctor` and
registers `/usr/sbin/pagespeed-doctor` with `update-alternatives`, so a host
with both the Apache and the nginx package has one command and no file
conflict. Its option tables are generated from the module source by
`test/doctor/doctor_tables.py`; `test/doctor/run-container-test.sh` runs it
against the public nginx package with a working and several broken
configurations.
