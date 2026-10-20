# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# OSS Scanner build of mod_pagespeed (https://github.com/We-Amp/mod_pagespeed).
# The scanner supplies the checkout as the build context and expects the built
# project at /src. The network is available while this file builds and is gone
# afterwards, so every dependency Bazel needs is fetched here.
#
# This mirrors the project's own development image (docker/Dockerfile): the
# Envoy build image for Bazel and the GCC 13 libstdc++, clang-20 on top, the
# Apache and nginx headers the modules compile against, and an nginx source
# tree at /usr/local/src/nginx that the ngx_pagespeed build rule reads.

# envoyproxy/envoy-build-ubuntu, pinned by digest (same pin as docker/Dockerfile).
FROM envoyproxy/envoy-build-ubuntu@sha256:1b3c82ca34c505c4951918b2e0a0c3db88cf266ebbf4196e4b0fba8fa137ada3

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV DEBIAN_FRONTEND=noninteractive \
    PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    BAZELISK_HOME=/bazel-cache/bazelisk

# clang-20: abseil's absl_nonnull needs nullability_on_classes support that the
# image's default clang lacks. Replace the default clang in /opt/llvm/bin so
# Bazel's CC=clang picks it up.
RUN apt-get update && apt-get install -y --no-install-recommends \
      software-properties-common gnupg wget \
    && . /etc/os-release \
    && wget -qO /usr/share/keyrings/llvm-snapshot.gpg.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-20 main" \
       > /etc/apt/sources.list.d/llvm-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
      clang-20 libc++-20-dev libc++abi-20-dev lld-20 libclang-rt-20-dev \
    && ln -sf /usr/bin/clang-20 /opt/llvm/bin/clang \
    && ln -sf /usr/bin/clang++-20 /opt/llvm/bin/clang++ \
    && ln -sf /usr/bin/clang-20 /opt/llvm/bin/clang-20 \
    && ln -sf /usr/bin/clang++-20 /opt/llvm/bin/clang++-20 \
    && ln -sf /usr/bin/lld-20 /opt/llvm/bin/lld \
    && rm -rf /var/lib/apt/lists/*

# Build and test dependencies of the modules: Apache httpd headers (mod_pagespeed.so
# and the Apache system tests), flex/bison (libmemcached), the AV1 stack's build
# tools (nasm for libaom, meson + ninja for dav1d), gperf, uuid, Java for the
# Closure compiler, nginx and its configure dependencies, and sudo so the dev
# user can run the server-based system tests.
RUN apt-get update && apt-get install -y --no-install-recommends \
      apache2 apache2-dev flex bison nasm meson ninja-build redis-tools gperf uuid-dev \
      python3-pip python3-distutils default-jre-headless \
      nginx libpcre3-dev zlib1g-dev libssl-dev sudo gdb \
    && rm -rf /var/lib/apt/lists/*

# Node.js 20 and the Closure compiler, which the JavaScript genrules invoke.
RUN curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/nodesource.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/nodesource.gpg] https://deb.nodesource.com/node_20.x nodistro main" \
        > /etc/apt/sources.list.d/nodesource.list \
    && apt-get update && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/* \
    && npm install -g google-closure-compiler@20260317.0.0

# Python packages the system tests use: what the project's own development
# image pins (this image's Python predates pytest 9) plus the timeout plugin
# the tests' markers expect.
RUN pip3 install --no-cache-dir --break-system-packages requests==2.32.4 pytest==8.3.5 pytest-timeout==2.3.1 2>/dev/null || \
    pip3 install --no-cache-dir requests==2.32.4 pytest==8.3.5 pytest-timeout==2.3.1

# nginx source, configured and built once: the nginx_repository Bazel rule reads
# /usr/local/src/nginx, and the built binary serves the nginx system tests.
ENV NGINX_VERSION=1.30.5
ENV NGINX_SHA256=6c20565aa2325cb82216ae804f4a4ff1875179014759a381c42ddc8e11c4906d
RUN curl -fsSL "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" -o /tmp/nginx.tar.gz \
    && echo "${NGINX_SHA256}  /tmp/nginx.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/nginx.tar.gz -C /usr/local/src \
    && rm /tmp/nginx.tar.gz \
    && mv "/usr/local/src/nginx-${NGINX_VERSION}" /usr/local/src/nginx \
    && cd /usr/local/src/nginx \
    && ./configure --with-cc=/opt/llvm/bin/clang \
        --with-http_v2_module --with-http_ssl_module --with-threads --with-compat \
    && make -j"$(nproc)"

# Bazel's hermetic Python toolchain refuses to run as root, so the build runs as
# an unprivileged user with passwordless sudo. The Bazel output base and the
# bazelisk download live under /bazel-cache, shared by every user of the image.
# A `bazel` wrapper ahead of bazelisk on PATH re-executes as `dev` when invoked
# as root, so a root shell can type `bazel build` or `bazel test` and reuse this
# build (from a directory dev can read, such as /src); sudo keeps BAZELISK_HOME
# so bazelisk finds the Bazel binary offline, and the caller's PATH is carried
# across sudo's secure_path reset. git is told that /src, owned by dev, is safe
# for root, so `git diff` and `git status` work for the scanner's agent.
RUN groupadd -g 1000 dev \
    && useradd -m -u 1000 -g 1000 -s /bin/bash dev \
    && printf '%s\n' 'dev ALL=(ALL) NOPASSWD:ALL' 'Defaults env_keep += "BAZELISK_HOME"' 'Defaults !fqdn' > /etc/sudoers.d/dev \
    && chmod 0440 /etc/sudoers.d/dev \
    && echo 'BAZELISK_HOME=/bazel-cache/bazelisk' >> /etc/environment \
    && echo 'export BAZELISK_HOME=/bazel-cache/bazelisk' > /etc/profile.d/bazelisk.sh \
    && mkdir -p /bazel-cache \
    && chown -R dev:dev /bazel-cache \
    && git config --system --add safe.directory /src \
    && printf '%s\n' '#!/bin/bash' \
         '# Bazel in this image runs as the unprivileged `dev` user (hermetic Python' \
         '# refuses root, and the output base is owned by dev). Root is re-executed as dev;' \
         '# run it from a directory dev can read, such as /src.' \
         'real=/usr/local/bin/bazel' \
         'if [ "$(id -u)" = 0 ]; then' \
         '  exec sudo -u dev -H env PATH="$PATH" "$real" "$@"' \
         'fi' \
         'exec "$real" "$@"' > /usr/local/sbin/bazel \
    && chmod 0755 /usr/local/sbin/bazel \
    && test -x /usr/local/bin/bazel

# scanner contract: the checkout lives inside the image, at /src
COPY --chown=dev:dev . /src
WORKDIR /src

# The repository's .bazelrc imports user.bazelrc last. Putting the scanner's
# settings there means a plain `bazel build` or `bazel test` inside the image
# reuses this build: the in-image output base, the project's preferred
# toolchain config, a pinned action PATH (so a shell with a different PATH
# does not invalidate the cache), debug info and frame pointers, no remote
# cache, and no Redis or Memcached ports (the project's bazelrc points the
# cache tests at local services; with the ports empty those tests skip).
RUN printf '%s\n' \
      'startup --output_user_root=/bazel-cache/out' \
      'build --config=clang-libstdcxx13' \
      'build --action_env=CC=clang --action_env=CXX=clang++' \
      'build --action_env=PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin' \
      'build --copt=-g --copt=-fno-omit-frame-pointer --strip=never' \
      'build --remote_cache=' \
      'test --test_output=errors' \
      'test --test_env=REDIS_PORT= --test_env=MEMCACHED_PORT=' \
      > /src/user.bazelrc \
    && chown dev:dev /src/user.bazelrc

USER dev
ENV HOME=/home/dev

# Build the two Linux modules, the smoke slice of the unit tests and the fuzz
# harnesses, run the smoke tests (a failing test does not abort the build) and
# drop Bazel's sandbox, all in one layer: a later layer cannot shrink an earlier
# one. The compile cache stays, so the rest of the unit suite
# (test/pagespeed/..., test/net/...), the sanitizer configs and the server-based
# system tests build offline from the cached engine objects; threat_model.md
# says how. A failing build fails the image and primary_contact gets the log.
RUN bazel build -- \
      //:libmod_pagespeed.so \
      //pagespeed/nginx:ngx_pagespeed_module.so \
      //test/pagespeed/kernel/base/... //test/pagespeed/kernel/html/... \
      //pagespeed/kernel/html:html_fuzz //pagespeed/kernel/js:js_minify_fuzz \
      //pagespeed/kernel/packs:pack_loader_fuzz //third_party/css_parser:parser_fuzz \
    && { bazel test -- //test/pagespeed/kernel/base/... //test/pagespeed/kernel/html/... \
          || echo "WARNING: bazel test reported failures; see /src/bazel-testlogs"; } \
    && { bazel shutdown || true; } \
    && rm -rf /bazel-cache/out/*/sandbox /bazel-cache/out/*/java.log* \
    && du -sh /bazel-cache
