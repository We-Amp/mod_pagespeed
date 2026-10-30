# mod_pagespeed threat model

## What this project does and where untrusted input enters

mod_pagespeed is a set of web-server modules that rewrite HTML responses in
flight and optimize the resources those pages reference: images are
recompressed, resized and converted (WebP, AVIF), CSS and JavaScript are
minified, combined and inlined, and the results are served under
`.pagespeed.`-encoded URLs with long cache lifetimes. This repository holds the
shared engine and the Apache (`mod_pagespeed.so`), nginx
(`ngx_pagespeed_module.so`), IIS and Envoy ports. The Linux image built here
contains the engine, the Apache module and the nginx module.

The modules run inside the web server's worker processes, so a memory-safety
bug in the engine is a bug in the server. Treat the following as
attacker-controlled:

- Every byte of origin content the server proxies or serves: HTML, CSS,
  JavaScript, images (JPEG, PNG, GIF, WebP, AVIF) and the headers that come
  with them. A site that uses mod_pagespeed in front of user-generated content,
  or whose origin is compromised, delivers hostile content straight into the
  rewriters. Malformed, truncated, deeply nested, recursive and internally
  inconsistent inputs are all in scope.
- Client requests: URLs, query parameters and headers. This includes the
  `.pagespeed.` resource URLs (encoded filter ids, hashes, content type and
  original name), the `PageSpeed=` and `PageSpeedFilters=` query parameters
  and request headers, `Accept`, `User-Agent`, `Save-Data`, cookies used in
  configuration matching, and the beacon POSTs (`/mod_pagespeed_beacon`,
  `/ngx_pagespeed_beacon`) that carry critical-image and critical-CSS data
  from browsers.
- Responses to the server's own fetches. The engine fetches resources through
  libcurl (`pagespeed/system`, `CurlUrlAsyncFetcher`) from the origin and from
  domains the configuration authorizes. The fetched bytes are untrusted; the
  set of fetchable hosts is a security boundary (see severity).
- Web Bot Auth material: HTTP message signature headers on incoming requests
  and the JSON key directories fetched from crawler operators
  (`pagespeed/kernel/webbotauth`).
- The on-disk cache. The Cyclone cache file is shared between the server's
  worker processes and is opened read-write by the module
  (`pagespeed/kernel/cache`, `pagespeed/kernel/cache/cyclone`). Treat its
  contents as possibly corrupted; a corrupted cache must not lead to memory
  corruption in the server.
- Rule packs (`pagespeed/kernel/packs`). The operator names pack files in the
  configuration, so the source is trusted, but the loader parses a file format
  and must fail safely on malformed packs.

Trusted: the server configuration directives, files the operator points the
server at (`LoadFromFile` mappings are trusted paths, traversal out of them is
not), the memcached and Redis backends the operator configures, and local
shared memory between workers of one server (`pagespeed/kernel/sharedmem`).

The embedding server (Apache, nginx) is assumed to honour its API contracts.
A crash that needs the server to hand the module an invalid pointer or
violate its own lifecycle is not a finding by itself; hostile content or a
hostile request that makes the module violate those contracts is.

## Components that matter most and least

Highest value, roughly in order of exposure:

- The HTML parser and lexer (`pagespeed/kernel/html`,
  `net/instaweb/htmlparse`) and the rewriter filters
  (`net/instaweb/rewriter`), which see every response byte.
- The CSS parser in `third_party/css_parser` (maintained in this repository,
  first-party for scoping purposes), CSS filters (combine, flatten imports,
  inline, minify, image rewriting inside CSS) and the JavaScript minifier
  (`pagespeed/kernel/js`, `net/instaweb/js`).
- Image handling (`pagespeed/kernel/image`): the format detection, the
  resizing and recompression paths and our use of the codecs. Bugs inside
  libjpeg-turbo, libpng, libwebp, giflib, libavif, libaom and dav1d belong to
  those projects; our misuse of their APIs (wrong sizes, unchecked return
  values, integer overflow in dimension arithmetic) is ours.
- URL handling: `.pagespeed.` URL encoding and decoding (`net/instaweb/rewriter`
  resource namer, URL partnership and input resource code),
  `pagespeed/kernel/http`, and our use of googleurl (an external Bazel
  repository).
  Decoding is where a client can steer the server towards a fetch, a cache key
  or a file.
- The HTTP cache layer and the Cyclone wrapper (`pagespeed/kernel/cache`,
  `pagespeed/kernel/cache/cyclone`), the shared-memory structures
  (`pagespeed/kernel/sharedmem`) and the daemon adapter in
  `pagespeed/system` that talks to the optimizer daemon over a socket.
- The admin and statistics handlers (`pagespeed/system/admin_site.cc`,
  `pagespeed/system/admin_daemon_handler.cc`, console, messages, rule-pack
  views): request routing, escaping of anything reflected into HTML, and the
  beacon handlers. Access control for these handlers is delegated by design
  to the web server's ACL on the handler locations plus the
  `AdminDomains`/`StatisticsDomains` family of options; the module warns when
  a handler is reachable from a non-loopback client.
  A finding is module behaviour that gets around a correctly configured ACL,
  for example an admin handler reachable through a URL the `<Location>` or
  `location` block does not cover; an unconfigured ACL is operator
  configuration, not a finding.
- Web Bot Auth verification (`pagespeed/kernel/webbotauth`): signature
  parsing, key directory parsing, caching and the network fetch of
  directories.
- The server glue in `pagespeed/apache`, `pagespeed/nginx` and
  `pagespeed/system`: request and response header handling, body streaming,
  flush and buffering limits, and process and thread lifecycle (the engine is
  multi-threaded, so thread safety on the request path is in scope).

Lower priority or out of scope:

- `html/` is a frozen archive of the 1.0-era documentation and is not shipped;
  ignore it.
- `pagespeed/iis` and `pagespeed/envoy` are not built in this image (IIS is
  Windows-only, the Envoy filter needs the Envoy tree). A finding there is
  welcome but needs its configuration and reachability explained.
- `aspnetcore/` is a separate .NET middleware with its own tests; it is not
  built here.
- `install/`, `packaging/`, `benchmark/`, `tools/` and the `test/` harnesses:
  a crash confined to a test or a build tool is not a product vulnerability.
- Third-party code under `third_party/` and in Bazel external repositories:
  report upstream bugs upstream; our use of them is in scope. Attribute each
  finding to the component holding its root cause.

## How to exercise it

This section describes the scanner's build image; outside it, see the
repository's build documentation.

The checkout is built at `/src`. `/src/user.bazelrc` carries the settings of
this image (in-image output base, the project's clang toolchain config,
line-table debug info and frame pointers, no remote cache), so plain `bazel`
commands reuse the build without network access, for example
`bazel test //test/pagespeed/kernel/base/...`. Stay in `/src` and in its
output base: a different `--output_base`, or a copy of the checkout (the
output base is keyed on the workspace path), refetches the git dependencies
`@cyclone` and `@aom_src`, which is impossible offline. Backtraces carry file
and line. For variables in gdb, add full debug info to the files you are
looking at only, for example
`--per_file_copt=pagespeed/kernel/html/.*@-g`; a global `--copt=-g` would
recompile everything and replace the cached build, including the prebuilt
module the system tests load.

Bazel runs as the unprivileged `dev` user (the hermetic Python toolchain
refuses root and the output base belongs to `dev`); the `bazel` on PATH is a
wrapper that re-executes as `dev` when called from a root shell, so plain
`bazel` works everywhere. `sudo -u dev` is passwordless if you need it
directly.

- Unit tests live under `test/pagespeed/...` and `test/net/...`, one tree per
  engine component. The image has not built any of them; the engine objects
  every suite links against are in the cache, so `bazel test` of a target
  compiles its test sources and links offline. Start with
  `//test/pagespeed/kernel/base/...` and `//test/pagespeed/kernel/html/...`;
  the first of them also compiles googletest, which nothing prebuilt in the
  image needed. Test one target at a time
  (`bazel test //test/pagespeed/kernel/image/...`), not the whole tree:
  compiling test sources takes a long time on a small machine, and
  `//test/net/instaweb/rewriter:rewriter` is one binary of 119 test files,
  the most expensive of them. The Redis- and Memcached-backed cache tests skip unless you pass
  `--test_env=REDIS_PORT=<port>` or `--test_env=MEMCACHED_PORT=<port>` for a
  running service (the image has neither server). `curl_url_async_fetcher_test`
  is hermetic (an in-process server on 127.0.0.1) and exercises the fetch path
  offline.
- Sanitizer builds work offline: `bazel test --config=clang-asan <target>`
  (AddressSanitizer plus UndefinedBehaviorSanitizer) and
  `--config=clang-tsan`. Every external dependency is already fetched, but a
  sanitizer build recompiles everything the target depends on into its own
  output directory, so build one harness or one test at a time
  (`bazel build --config=clang-asan //pagespeed/kernel/html:html_fuzz`), not
  the tree.
- Fuzz harnesses, already built in the image, each with a seed corpus in the
  tree. Run the binaries from `bazel-bin` so relative corpus paths resolve
  (`bazel run` changes into the runfiles directory):
  `bazel-bin/pagespeed/kernel/html/html_fuzz tools/html-parse-corpus/seeds/*`
  for the HTML parser, `bazel-bin/pagespeed/kernel/js/js_minify_fuzz` for the
  JavaScript minifier (dual-oracle: idempotence plus token-stream
  equivalence),
  `bazel-bin/pagespeed/kernel/packs/pack_loader_fuzz tools/pack-loader-corpus/seeds/*`
  for rule packs and `bazel-bin/third_party/css_parser/parser_fuzz` for the
  CSS parser. The prebuilt harnesses are debug builds with DCHECKs on; a
  DCHECK that fires is a lead, and a finding when the release build (add
  `-c opt`) behaves unsafely on the same input. Rebuild with
  `bazel build --config=clang-asan -c opt <target>` to run a harness under the
  sanitizers. Each harness can also be built as a libFuzzer target; the BUILD
  files next to them say which define and `-fsanitize=fuzzer` flag to add.
- System tests run a real server with the module loaded and drive it with
  pytest over HTTP. Run them as `dev`, not root: a server started by root
  drops its workers to `nobody`, which cannot write the test log directory,
  and ten log-based tests then fail for that reason alone.
  `sudo -u dev -H bash -lc 'cd /src && ./test/system/run_nginx_tests.sh --skip-build'`
  starts the nginx built at `/usr/local/src/nginx` on port 8080 with the
  prebuilt module and the test site under `test/system`, and runs the
  `automatic/`, `system/` and `nginx/` suites (about 590 tests in a few
  minutes). `./test/system/run_system_tests.sh --skip-build` does the same
  for Apache on port 80 (`apachectl` runs through `sudo`). Pass `--skip-build`
  in both cases: the Apache script's own build step defaults to a gcc
  toolchain config this image does not carry. Either server is a convenient
  target for a proof of concept: start it with `--keep-running` and send
  requests with curl.
- The built artifacts are `bazel-bin/libmod_pagespeed.so` and
  `bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so`.

## How we rate severity

- **Critical**: code execution or memory corruption in the server process
  that a remote client or origin content can control; an authentication or
  access-control bypass that exposes the admin handlers' write actions to a
  remote client.
- **High**: any memory-safety bug (out-of-bounds read or write, use after
  free, double free, type confusion, uninitialized memory reaching output)
  reachable from untrusted content or requests, with or without a
  demonstrated exploit; a single request or a single origin response that
  crashes or hangs a server worker; a fetch to a host outside the configured
  domain set (server-side request forgery); cache poisoning, meaning content
  served under a URL or cache key it does not belong to; a path traversal out
  of a `LoadFromFile` directory; a data race on request-path objects that
  leads to any of the above.
- **Medium**: denial of service that needs sustained traffic or very large
  inputs (algorithmic complexity, unbounded buffering or memory growth);
  information disclosure through statistics or admin pages beyond what the
  default access restriction already shows the operator; HTML or script
  injection in an admin page; integer overflows with no demonstrated memory
  effect; a per-request leak (a leak that grows without bound under ordinary
  traffic is High).
- **Low**: issues that need a hostile configuration or a file the operator
  placed; findings only in tests, tools or benchmarks; findings in ports not
  built in this image without a reachability argument.
- **Not findings on their own**: anything that requires the operator to be
  the attacker, local root, or a writable server filesystem; upstream codec or
  library bugs without a path through this project's use of them; a crash
  that needs the embedding server to break its API contract.

## How we would like reports and patches

- One finding per report, with the root cause in the component that owns it,
  the input that triggers it, and the shortest reproduction: a unit-test
  input, a fuzz harness invocation, or a curl command against the nginx
  system-test server. Attach the sanitizer trace when there is one.
- A patch is welcome. Keep it minimal, in the style of the surrounding code,
  formatted with clang-format (`tools/format.sh`), with a regression test in
  the matching `test/` tree. A merge-ready patch is better than a sketch, but
  a sketch with a correct root-cause analysis is better than a broad rewrite.
- We rate reports by the rubric above. A report that argues its severity from
  the rubric saves a round trip.

## Anything to leave alone

- The `html/` documentation archive.
- Two nginx system tests with known test-harness flakiness,
  `TestLaneFixtureGate::test_known_names_are_pinned` and
  `TestConsoleJsonHandler::test_json_handler_returns_the_logged_series`;
  failures in them are not module findings.
- Thread-safety findings confined to test fixtures or to the single-threaded
  command-line tools.
