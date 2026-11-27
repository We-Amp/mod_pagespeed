# Implementation Prompt — Production Readiness Test Suite

## Context

You are implementing a production readiness E2E test suite for mod_pagespeed 2.1, a
caching reverse proxy (nginx module + worker daemon). The test suite lives in
`tools/production-tests/`.

Read `tools/production-tests/DESIGN.md` for the full architecture and test
specification. Read `CLAUDE.md` for project conventions.

## Existing Infrastructure to Reference

Before writing any code, study these existing implementations:

- `tools/http-compliance/compliance_origin.py` — Existing HTTP compliance origin
  server (FastAPI, 496 lines). Has conditional requests, range, cache-control,
  chunked, error routes. **Extend or draw from this pattern.**
- `tools/http-compliance/conftest.py` — ComplianceClient with poll_for_hit,
  assert_miss, assert_hit, method helpers
- `tools/stress/metrics_helpers.py` — IPC socket queries (stats, health, purge)
- `tools/stress/conftest.py` — Session-scoped fixtures, warm_cache
- `tools/e2e/conftest.py` — Docker Compose lifecycle management
- `tools/e2e/docker-compose.yml` — 3-service stack pattern
- `tools/e2e/Dockerfile.e2e` — Combined nginx+worker build
- `tools/e2e/nginx-e2e.conf` — Nginx config with pagespeed directives

## What to Build

### Phase 1: Infrastructure (origin, Docker, fixtures)

1. **Dynamic Origin Server** (`origin/main.py`)
   - FastAPI + uvicorn
   - All routes from DESIGN.md (status codes, cache-control, chunked, range,
     conditional, encoding, sized responses, disconnect, headers, content types)
   - Request counter endpoint for verifying origin hits
   - Health check endpoint
   - `origin/Dockerfile` + `origin/requirements.txt`
   - `origin/testdata/` with small test files (generate programmatically where possible)

2. **Docker Compose** (`docker-compose.yml`)
   - 3 services: origin, worker, nginx
   - Shared named volume at `/shared`
   - Reuse existing Dockerfile.e2e pattern for nginx+worker
   - Origin as separate Python service
   - Health checks on all services

3. **Nginx Config** (`nginx.conf`)
   - pagespeed on, cache path, worker socket
   - proxy_pass to origin:8081
   - Appropriate timeouts for streaming tests
   - Access log with request timing

4. **Test Fixtures** (`conftest.py`)
   - `ProductionTestClient` class (see DESIGN.md)
   - Session-scoped Docker Compose lifecycle
   - `client` fixture (nginx proxy)
   - `origin_client` fixture (direct origin)
   - `metrics_client` fixture (worker stats via docker exec)
   - `purge` fixture (cache purge helper)

5. **Run Script** (`run.sh`)
   - Build, start stack, wait for health, run pytest, collect logs on failure

### Phase 2: Test Modules (10 files)

Each test file should:
- Have clear docstring explaining what it tests
- Use pytest markers for priority: `@pytest.mark.p0`, `@pytest.mark.p1`, `@pytest.mark.p2`
- Be independent (no cross-module state)
- Use descriptive test names matching DESIGN.md
- Include comments explaining non-obvious assertions

Implement ALL tests from DESIGN.md (175 tests across 10 modules).

## Implementation Guidelines

### Origin Server

```python
# Pattern for status code routes
@app.get("/status/{code}")
async def status_code(code: int, body: str = "", cache: int = 0,
                      headers: str = ""):
    resp_headers = {}
    if cache > 0:
        resp_headers["Cache-Control"] = f"max-age={cache}"
    if headers:
        for h in json.loads(headers):
            resp_headers[h["name"]] = h["value"]
    content = body.encode() if body else f"Status {code}".encode()
    return Response(content=content, status_code=code, headers=resp_headers)

# Pattern for chunked streaming
@app.get("/chunked/{pattern}")
async def chunked(pattern: str, delay: float = 0, size: int = 1024):
    async def generate():
        if pattern == "small":
            for i in range(size):
                yield bytes([ord('a') + (i % 26)])
        elif pattern == "large":
            chunk = b"x" * (1024 * 1024)
            for _ in range(5):
                yield chunk
        elif pattern == "slow":
            for _ in range(10):
                yield b"x" * 1024
                await asyncio.sleep(delay or 2)
        elif pattern == "empty":
            return  # Zero-length body
    return StreamingResponse(generate(), media_type="text/css")

# Pattern for conditional requests
@app.get("/conditional/{resource_id}")
async def conditional(resource_id: str, request: Request):
    body = f"Resource {resource_id} content".encode()
    etag = f'W/"{hashlib.md5(body).hexdigest()[:8]}"'

    if_none_match = request.headers.get("if-none-match", "")
    if if_none_match:
        # Weak comparison for If-None-Match
        client_etags = [e.strip() for e in if_none_match.split(",")]
        if etag in client_etags or "*" in client_etags:
            return Response(status_code=304, headers={"ETag": etag})

    return Response(
        content=body,
        headers={
            "ETag": etag,
            "Cache-Control": "max-age=3600",
            "Content-Type": "text/plain",
        },
    )

# Pattern for disconnect mid-response
@app.get("/disconnect/mid")
async def disconnect_mid(request: Request, after: int = 1000):
    """Send `after` bytes then close connection."""
    async def generate():
        yield b"x" * after
        # Force close by raising
        raise Exception("Intentional disconnect")
    return StreamingResponse(
        generate(),
        media_type="application/octet-stream",
        headers={"Content-Length": str(after * 10)},  # Lie about length
    )
```

### Test Patterns

```python
# Pattern: Verify status code forwarding
class TestStatusCodes:
    @pytest.mark.p0
    def test_200_ok_cached(self, client):
        """200 OK from origin should be cached on second request."""
        path = "/status/200?cache=3600&body=hello"
        r1 = client.get(path)
        assert r1.status_code == 200
        client.assert_miss(r1)

        r2 = client.poll_for_hit(path)
        assert r2.status_code == 200
        assert r2.text == "hello"

    @pytest.mark.p0
    def test_404_not_cached_default(self, client):
        """404 without Cache-Control should not be cached."""
        path = f"/status/404?body=not-found-{uuid4().hex[:8]}"
        r1 = client.get(path)
        assert r1.status_code == 404
        client.assert_miss(r1)

        time.sleep(2)
        r2 = client.get(path)
        assert r2.status_code == 404
        client.assert_miss(r2)

# Pattern: Verify conditional requests
class TestConditionalRequests:
    @pytest.mark.p0
    def test_if_none_match_304(self, client):
        """Matching weak ETag returns 304 Not Modified."""
        path = "/content/cacheable-resource"
        r1 = client.poll_for_hit(path)
        etag = r1.headers["ETag"]
        assert etag.startswith('W/"ps-')

        r2 = client.get(path, headers={"If-None-Match": etag})
        assert r2.status_code == 304
        assert len(r2.content) == 0

# Pattern: Verify data integrity under load
class TestStressIntegrity:
    @pytest.mark.p0
    def test_response_body_hash_under_load(self, client):
        """Every response body matches expected hash under concurrent load."""
        from concurrent.futures import ThreadPoolExecutor, as_completed

        # Pre-populate cache
        paths = [f"/content/integrity-{i}" for i in range(100)]
        expected = {}
        for path in paths:
            r = client.poll_for_hit(path)
            expected[path] = hashlib.sha256(r.content).hexdigest()

        # Concurrent verification
        errors = []
        def verify(path):
            for _ in range(10):
                r = client.get(path)
                h = hashlib.sha256(r.content).hexdigest()
                if h != expected[path]:
                    errors.append(f"{path}: expected {expected[path]}, got {h}")

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(verify, p) for p in paths]
            for f in as_completed(futures):
                f.result()

        assert not errors, f"Hash mismatches: {errors}"
```

### Fixtures

```python
# conftest.py
import pytest
import subprocess
import time
import requests

class ProductionTestClient:
    def __init__(self, base_url):
        self.base_url = base_url
        self.session = requests.Session()

    def get(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.get(f"{self.base_url}{path}", **kwargs)

    def head(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.head(f"{self.base_url}{path}", **kwargs)

    def options(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.options(f"{self.base_url}{path}", **kwargs)

    def poll_for_hit(self, path, max_attempts=30, interval=1, **kwargs):
        """Poll until X-PageSpeed: HIT is received."""
        for i in range(max_attempts):
            r = self.get(path, **kwargs)
            if r.headers.get("X-PageSpeed") == "HIT":
                return r
            time.sleep(interval)
        pytest.fail(f"Never got HIT for {path} after {max_attempts} attempts")

    def assert_miss(self, response):
        assert response.headers.get("X-PageSpeed") == "MISS", \
            f"Expected MISS, got {response.headers.get('X-PageSpeed')}"

    def assert_hit(self, response):
        assert response.headers.get("X-PageSpeed") == "HIT", \
            f"Expected HIT, got {response.headers.get('X-PageSpeed')}"

    def assert_not_cached(self, path, attempts=3, interval=2):
        """Verify path stays MISS across multiple requests."""
        for _ in range(attempts):
            r = self.get(path)
            self.assert_miss(r)
            time.sleep(interval)

    def assert_content_length_matches(self, response):
        cl = response.headers.get("Content-Length")
        if cl is not None:
            assert int(cl) == len(response.content), \
                f"Content-Length {cl} != actual {len(response.content)}"


@pytest.fixture(scope="session")
def stack():
    """Start Docker Compose stack for the session."""
    compose_dir = os.path.dirname(__file__)
    subprocess.run(
        ["docker", "compose", "up", "-d", "--build", "--wait"],
        cwd=compose_dir, check=True, timeout=120
    )
    # Wait for health
    for _ in range(30):
        try:
            r = requests.get("http://localhost:8081/health", timeout=2)
            if r.status_code == 200:
                break
        except:
            pass
        time.sleep(1)
    yield
    # Optionally tear down (leave running for debugging)


@pytest.fixture
def client(stack):
    return ProductionTestClient("http://localhost:8080")

@pytest.fixture
def origin_client(stack):
    return ProductionTestClient("http://localhost:8081")
```

## Parallel Agent Execution Plan

This implementation can be split across 5 parallel agents:

### Agent 1: Infrastructure
- `origin/main.py` (all routes)
- `origin/Dockerfile`, `origin/requirements.txt`
- `docker-compose.yml`
- `Dockerfile.nginx`, `Dockerfile.worker`, `nginx.conf`
- `run.sh`
- `conftest.py`, `helpers/client.py`, `helpers/metrics.py`, `helpers/assertions.py`

### Agent 2: Protocol Tests
- `tests/test_status_codes.py` (20 tests)
- `tests/test_conditional_requests.py` (15 tests)
- `tests/test_cache_control.py` (18 tests)

### Agent 3: Transfer & Negotiation Tests
- `tests/test_range_requests.py` (12 tests)
- `tests/test_content_negotiation.py` (16 tests)
- `tests/test_streaming_transfer.py` (25 tests)

### Agent 4: Security & Headers Tests
- `tests/test_headers_security.py` (22 tests)

### Agent 5: Stress, Deployment & Observability Tests
- `tests/test_stress_integrity.py` (20 tests)
- `tests/test_deployment_config.py` (15 tests)
- `tests/test_observability.py` (12 tests)

## Key Conventions

1. **Use unique paths per test** — Append UUID or test name to avoid cache
   interference between tests
2. **Use `poll_for_hit()` for worker-dependent tests** — Worker processes async
3. **Mark priorities** — `@pytest.mark.p0`, `@pytest.mark.p1`, `@pytest.mark.p2`
4. **Test independence** — Each test should work in isolation
5. **Descriptive failures** — Include context in assertion messages
6. **Timeout safety** — All network calls have timeouts
7. **Docstrings** — Every test has a one-line docstring explaining what it validates
