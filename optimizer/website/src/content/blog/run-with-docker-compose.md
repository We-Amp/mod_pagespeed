---
title: 'Run ModPageSpeed 2.0 with Docker Compose'
description: 'A working docker-compose.yml for ModPageSpeed 2.0 on nginx: worker + interceptor + shared Cyclone cache volume. Verify with curl, done.'
date: 2026-05-20
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['installation', 'docker', 'nginx']
draft: false
product: '2.0'
---

> Written for the ModPageSpeed 2.0 line: the image tags and the `pagespeed_*` directives below are 2.0's. For mod_pagespeed, follow [the Docker install guide](/docs/installation-docker/).

mod_pagespeed ships as two cooperating containers. One is a worker
that transcodes images and minifies assets; the other is an nginx interceptor
that serves the optimized variants. Both processes share a single
[Cyclone cache](/blog/cyclone-cache-vs-file-cache-benchmark/) volume via
memory-mapped I/O. What follows is the shortest path from a fresh host to a
working install.

## Prerequisites

- Docker 24+ with Compose v2 (`docker compose`, not `docker-compose`).
- An origin (your existing app) reachable from the nginx container. By
  default the compose file assumes `host.docker.internal:8081`.

## docker-compose.yml

This is the production compose file from the mod_pagespeed repo, trimmed
for readability:

```yaml
services:
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.8.1}
    volumes:
      - pagespeed-data:/data
    environment:
      - CACHE_SIZE=${CACHE_SIZE:-1073741824} # 1 GiB
    restart: unless-stopped
    healthcheck:
      test:
        ['CMD-SHELL', "socat -u -T 4 UNIX-CONNECT:/data/pagespeed.sock.health - | grep -q '^OK'"]
      interval: 10s
      timeout: 5s
      retries: 3
      start_period: 5s

  nginx:
    image: ghcr.io/we-amp/pagespeed-nginx:${PAGESPEED_VERSION:-2.8.1}
    ports:
      - '${NGINX_PORT:-80}:80'
      - '${NGINX_SSL_PORT:-443}:443'
    volumes:
      - pagespeed-data:/data
      # Custom config: - ./nginx.conf:/etc/nginx/nginx.conf:ro
      # TLS:           - ./ssl/cert.pem:/etc/nginx/ssl/cert.pem:ro
      #                - ./ssl/key.pem:/etc/nginx/ssl/key.pem:ro
    environment:
      - BACKEND_HOST=${BACKEND_HOST:-host.docker.internal}
      - BACKEND_PORT=${BACKEND_PORT:-8081}
    extra_hosts:
      - 'host.docker.internal:host-gateway'
    depends_on:
      worker:
        condition: service_healthy
    restart: unless-stopped
    healthcheck:
      test: ['CMD', 'curl', '-sf', 'http://localhost/health']
      interval: 10s
      timeout: 5s
      retries: 3
      start_period: 3s

volumes:
  pagespeed-data:
    driver: local
```

Note:

- The shared named volume `pagespeed-data` is mounted at `/data` in both
  containers. That is where the Cyclone cache file lives. Both processes
  open it via `mmap`, so writes from the worker show up in the nginx
  interceptor's address space immediately. No IPC, no socket round-trips for
  cache reads.
- `extra_hosts: host.docker.internal:host-gateway` is what lets
  nginx in Docker reach an origin on the host on Linux. macOS and Windows
  Docker resolve this hostname by default; the explicit mapping is for
  Linux parity.

## .env

Drop a `.env` next to the compose file:

```bash
# Origin server (your app)
BACKEND_HOST=host.docker.internal
BACKEND_PORT=8081

# Cache size in bytes (1 GiB by default)
CACHE_SIZE=1073741824

# Image tag (pin in production)
PAGESPEED_VERSION=2.8.1
```

## Start it

```bash
$ docker compose up -d
[+] Running 3/3
 ✔ Network modpagespeed_default  Created
 ✔ Container modpagespeed-worker-1  Healthy
 ✔ Container modpagespeed-nginx-1   Started

$ docker compose ps
NAME                       STATUS                    PORTS
modpagespeed-nginx-1       Up 12s (healthy)          0.0.0.0:80->80/tcp, 0.0.0.0:443->443/tcp
modpagespeed-worker-1      Up 22s (healthy)
```

The `depends_on: condition: service_healthy` clause means nginx will not
start until the worker reports `OK` on its Unix-domain health socket. If
nginx never starts, check `docker compose logs worker` first.

## Verify

The fastest check is the response header. mod_pagespeed emits
`X-PageSpeed:` on optimized responses (the 1.x lineage uses
`X-Mod-Pagespeed:` on Apache and `X-Page-Speed:` on nginx and IIS, same
project family, different headers):

```bash
$ curl -sI http://localhost/ | grep -i pagespeed
x-pagespeed: HIT
```

For more diagnostic depth, hit the nginx health endpoint and read the
worker socket:

```bash
$ curl -s http://localhost/health
OK

$ docker compose exec worker socat - UNIX-CONNECT:/data/pagespeed.sock.health
OK 0/128
```

`0/128` is `queue_depth/queue_capacity`. A worker that is constantly at
`128/128` is bottlenecked on transcoding, usually a sign the cache volume
is too small and variants are being evicted faster than they're being
generated.

## Custom nginx.conf

The default `nginx.conf` baked into the image proxies everything to
`$BACKEND_HOST:$BACKEND_PORT`. To override it (multiple vhosts, TLS, more
specific `pagespeed_disallow` rules), mount your own:

```yaml
services:
  nginx:
    volumes:
      - pagespeed-data:/data
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
```

A minimal `nginx.conf` with PageSpeed enabled:

```nginx
load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

events { worker_connections 1024; }

http {
    include /etc/nginx/mime.types;

    upstream backend {
        server host.docker.internal:8081;
        keepalive 32;
    }

    server {
        listen 80;

        pagespeed on;
        pagespeed_cache_path /data/cache.vol;
        pagespeed_enable_mmap_directory on;

        # Don't optimize these
        pagespeed_disallow /api/;
        pagespeed_disallow /admin/;
        pagespeed_disallow "*.woff2";

        location / {
            proxy_pass http://backend;
            proxy_set_header Host $host;
            proxy_set_header Accept-Encoding "";  # PageSpeed needs raw bytes
            proxy_http_version 1.1;
        }

        location /health {
            access_log off;
            return 200 "OK\n";
        }
    }
}
```

Two non-obvious config items worth flagging:

- `proxy_set_header Accept-Encoding "";` (the origin must return
  uncompressed bytes). PageSpeed caches raw HTML/CSS/JS and applies
  brotli/gzip at serve time. If your origin gzips eagerly, PageSpeed sees
  compressed bytes and the HTML pipeline silently skips.
- `pagespeed_enable_mmap_directory on;` (required for shared-cache mode).
  Without it, the nginx interceptor falls back to socket round-trips for every
  read, which is functional but slow.

## Production checklist

Before pointing real traffic at this:

- Pin the image tag (`ghcr.io/we-amp/pagespeed-worker:2.8.1`, not `:latest`).
- Set `CACHE_SIZE` to ~10% of the variants you expect (rule of thumb: 1
  GiB per 5000 unique image URLs).
- Add a logging driver. `json-file` with rotation is the compose-file
  default but consider shipping to your aggregator.
- Set up TLS termination. The compose file leaves it off; mount your
  certs and uncomment the `ssl_*` directives in `nginx.conf`.
- Wire `/health` into your uptime monitor.
- Consider the full [installation walkthrough](/docs/installation-docker/)
  for production deploy detail.

## What next

- Install & run: it optimizes out of the box, and it is licensed under
  Apache-2.0 and free to run in development and in production — see the
  [license](/license/).
- [Getting started](/docs/getting-started/) — full installation overview
  including non-Docker paths.
- [Installation: Docker](/docs/installation-docker/) — production-grade
  Docker setup with volume sizing and tuning.
- [ASP.NET Core middleware](/blog/aspnet-core-middleware/) if you'd rather
  embed the optimizer than run nginx as a reverse proxy.
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
  for how the worker decides which CSS to inline.
- [mod_pagespeed alternatives](/alternatives/mod-pagespeed/) — short form
  on what changed between 1.x and 2.0.
- [vs imgproxy](/alternatives/imgproxy/) — comparison if you're evaluating image
  pipelines.
