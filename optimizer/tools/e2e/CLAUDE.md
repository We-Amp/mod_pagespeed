# E2E Test Suite

Docker Compose-based end-to-end tests validating the full nginx → worker pipeline.

## Key Files
- `run_e2e.sh` -- launches Docker stack and runs pytest
- `docker-compose.yml` -- 3 containers: origin, worker, nginx (shared named volume)
- `conftest.py` -- pytest fixtures (wait for services, HTTP helpers)
- `test_user_stories.py` -- user story tests (HTML rewriting, image optimization, CSS inlining)
- `test_browser.py` -- browser analysis tests (requires Chromium)
- `run_browser_tests.sh` -- browser test launcher
- `origin_server.py` -- Python origin server serving test fixtures
- `nginx-e2e.conf` -- nginx config for E2E environment
- `testdata/` -- test HTML, CSS, images

## Running
```bash
./tools/e2e/run_e2e.sh                                   # Full suite
cd tools/e2e && pytest test_user_stories.py -v -k "css"   # Specific test
```

## Architecture

Docker Compose creates: origin (:8081), worker (:9881), nginx (:8083).
All share a named volume for the Cyclone cache. Tests hit nginx and verify
optimized responses (format conversion, CSS inlining, HTML rewriting).

## Gotchas
- `docker compose down -v` required for fresh cache (named volume persists).
- Browser tests need Chromium in the container.
- Tests tagged `no_tsan` -- not for sanitizer runs.
