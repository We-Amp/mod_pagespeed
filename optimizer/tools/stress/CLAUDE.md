# Stress Test Suite

Concurrent load and chaos testing for the nginx + worker stack.

## Key Files
- `run_stress.sh` -- launches Docker stack and runs pytest
- `docker-compose.yml` -- nginx (:8190), origin (:8191, nginx:1.26-alpine), ports configurable via STRESS_NGINX_PORT/STRESS_ORIGIN_PORT
- `conftest.py` -- pytest fixtures, service readiness checks
- `metrics_helpers.py` -- HTTP helpers (60s socket timeout for slow AVIF encodes)
- `ipc_client.py` -- IPC notification client for direct worker communication
- `test_http_load.py` -- concurrent HTTP load tests
- `test_cache_stress.py` -- cache fill/eviction under pressure
- `test_worker_stress.py` -- worker processing under load
- `test_proactive_variant.py` -- proactive variant generation tests
- `test_ipc_proof.py` -- IPC protocol stress tests
- `test_monitoring.py` -- stats/metrics endpoint tests
- `test_chaos.py` -- fault injection tests

## Running
```bash
./tools/stress/run_stress.sh
# Or with stack already running:
cd tools/stress && pytest -v
```

## Gotchas
- `docker compose down -v` required for fresh cache (named volume persists).
- Socket timeout is 60s in `metrics_helpers.py` to tolerate slow AVIF encodes.
- Origin must be nginx (not Python http.server) for concurrency.
- Port 8190 = nginx (STRESS_NGINX_PORT), 8191 = origin (STRESS_ORIGIN_PORT). Configurable via env vars.
- `STRESS_LATENCY_FACTOR` (float, default 1.0) scales the STATS latency bound in `test_event_loop_responsiveness.py`; the self-hosted nightly leg sets 2.0 for shared hosts.
