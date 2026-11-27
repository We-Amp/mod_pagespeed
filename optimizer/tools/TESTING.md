# Test Suites

This project has several test suites at different levels. Here's when to use each.

## Unit Tests (Bazel)

```bash
bazel test //test/...
```

Fast, no Docker required. Run these after any code change. See the
"What to Test After Changes" table in the root `CLAUDE.md`.

## E2E Tests (`tools/e2e/`)

```bash
./tools/e2e/run_e2e.sh
```

Full-stack integration: 3 Docker containers (origin, worker, nginx) sharing a
cache volume. Tests the complete request lifecycle: MISS → worker processing →
HIT serving. Covers HTML, image, CSS, JS optimization and cache behavior.

Key test files:
- `test_user_stories.py` — end-to-end user scenarios
- `conftest.py` — `PageSpeedClient` helper and Docker Compose fixtures

## HTTP Compliance Tests (`tools/http-compliance/`)

```bash
cd tools/http-compliance && docker compose up --build --abort-on-container-exit
```

Validates RFC 9111 caching behavior: Cache-Control header propagation,
conditional revalidation (304), no-store/private handling. Separate from E2E
because it uses a custom origin that sends specific cache headers.

Key test file: `test_cache_control.py`

## Production Tests (`tools/production-tests/`)

```bash
cd tools/production-tests && docker compose up --build --abort-on-container-exit
```

Tests against a realistic deployment configuration with multiple content types,
stress scenarios, and deployment verification. Includes cache warmup, variant
integrity, and stress integrity tests.

Key test files:
- `tests/test_deployment_config.py` — deployment configuration validation
- `tests/test_cache_control.py` — production cache behavior (distinct from http-compliance)
- `tests/test_stress_integrity.py` — variant integrity under load

## Stress Tests (`tools/stress/`)

```bash
cd tools/stress && docker compose up -d && pytest -v
```

Load testing and concurrency validation. Tests cache behavior under concurrent
requests, IPC protocol correctness, and variant generation throughput.

## Sanitizer Tests

```bash
./tools/docker-sanitizers.sh          # All sanitizers in Docker
bazel test --config=asan //...        # ASan + UBSan (native)
bazel test --config=tsan //...        # TSan (native)
```

Run before committing significant C/C++ changes. Docker sanitizers are the
most reliable (consistent libc++ environment).
