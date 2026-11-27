# mod_pagespeed 2.1 Load Test

Stress-tests the full nginx/cache/worker stack with concurrent HTTP requests.

## Prerequisites

- Python 3.8+
- `pip install requests`
- Docker Compose E2E stack running (`./tools/e2e/run_e2e.sh` or `docker compose -f tools/e2e/docker-compose.yml up -d`)

## Usage

```bash
# Burst test: 200 concurrent requests
python3 tools/load/load_test.py --mode burst --concurrency 200

# Sustained test: 50 req/s for 30 seconds
python3 tools/load/load_test.py --mode sustained --rps 50 --duration 30

# Custom target URL
python3 tools/load/load_test.py --mode burst --concurrency 100 --url http://localhost:8080
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--mode` | `burst` | `burst` or `sustained` |
| `--concurrency` | `100` | Concurrent requests (burst mode) |
| `--rps` | `20` | Requests per second (sustained mode) |
| `--duration` | `10` | Duration in seconds (sustained mode) |
| `--url` | `http://localhost:8080` | Target nginx URL |
| `--timeout` | `10` | Per-request timeout in seconds |

## What It Tests

Requests cycle through CSS, JS, HTML, and image paths. Image requests include
Accept header variants for WebP and AVIF negotiation. The report shows p50/p95/p99
latency, throughput, and error count.
