# Docker: install and verify mod_pagespeed (nginx reverse proxy)

Source: https://modpagespeed.com/docs/installation-docker/. Header: `X-PageSpeed`.
Scope: the containers in front of an existing HTTP origin. Run section 2 (one
container, for evaluation) or section 3 (the production stack, the worker and
nginx as separate services), not both: each publishes port 8080.

## 1. Prerequisites

Docker 24 or newer, Docker Compose v2, an origin the container can reach, host
port 8080 free, and nothing named `pagespeed` yet.

```bash
docker version --format '{{.Server.Version}}'
docker compose version
docker ps -a --filter 'name=^pagespeed$' --format '{{.Names}}'   # nothing
docker compose -p pagespeed ps -a -q                             # nothing
test -e pagespeed && echo 'a pagespeed directory exists here'    # nothing
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/   # 000: nothing listens on 8080
curl -s -o /dev/null -w '%{http_code}\n' 'http://<origin-host>:<origin-port>/'   # 200: the origin answers
```

Stop and ask if a `pagespeed` container, project or directory already exists or
port 8080 is taken. An origin on the Docker host must listen on `0.0.0.0` (or
the Docker bridge address), not only on `127.0.0.1`, for
`host.docker.internal` to reach it.

## 2. Install (one container)

```bash
docker run -d --name pagespeed -p 8080:80 \
  --add-host=host.docker.internal:host-gateway \
  -e BACKEND_HOST=host.docker.internal -e BACKEND_PORT=<origin-port> \
  ghcr.io/we-amp/pagespeed-combined:latest
```

`BACKEND_HOST` and `BACKEND_PORT` point at the origin; `host.docker.internal`
is the Docker host. For production, pin the image by digest
(`ghcr.io/we-amp/pagespeed-combined@sha256:<digest>`) instead of `:latest`.

## 3. Minimal configuration (production)

Work in a new directory so no existing `docker-compose.yml` or `nginx.conf` is
touched:

```bash
mkdir pagespeed && cd pagespeed
```

Create `docker-compose.yml` and `nginx.conf` as given in the source document,
which carries the current image tags, with two changes:

- Leave out the placeholder `origin` service, and `origin` from the worker's
  `depends_on`.
- Point `proxy_pass` at the real origin: `proxy_pass http://<origin-host>:<origin-port>;`.
  For an origin on the Docker host use `host.docker.internal` and add
  `extra_hosts: ['host.docker.internal:host-gateway']` to the `nginx` service.

Never overwrite an existing `docker-compose.yml` or `nginx.conf`. Then:

```bash
docker compose -p pagespeed up -d && docker compose -p pagespeed ps   # pidns, worker and nginx running
```

The nginx directives that matter are `pagespeed on;` and
`pagespeed_cache_path /shared/cache.vol;`; the worker and nginx share the cache
volume and one PID namespace, as that compose file sets up.

## 4. Verify

```bash
curl -s -o /dev/null -D - 'http://localhost:8080/?mps-verify=agent' | grep -i '^x-pagespeed:'
```

Pass: an `X-PageSpeed` line. `MISS` proves the module is active; `HIT` on a
later request means the optimized variant is cached, and is informational only:
optimization is asynchronous, and responses the origin marks uncacheable
(`Set-Cookie`, `private`, `no-store`) stay `MISS`
(https://modpagespeed.com/docs/troubleshooting/#cache-miss-on-every-request).
The header is added only to a 2xx response. No header: retry at most 3 times a
few seconds apart, then
https://modpagespeed.com/docs/installation-docker/#troubleshooting

## 5. Rollback

```bash
# Section 2: the single container and its image
docker rm -f pagespeed
docker rmi ghcr.io/we-amp/pagespeed-combined:latest

# Section 3: the stack and its images; run inside the pagespeed directory
# (cd into it first if the shell is not already there)
docker compose -p pagespeed down --rmi all
```

Do not add `-v` to `down` unless the operator asks: it deletes the cache
volume. Image tags and rolling back a release:
https://modpagespeed.com/docs/uninstall/#docker
