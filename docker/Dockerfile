# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Docker development environment for mod_pagespeed
# Based on Envoy's build image which includes clang, bazel, and most dependencies

# Tag: 86873047235e9b8232df989a5999b9bebf9db69c (pinned by digest for immutability)
FROM envoyproxy/envoy-build-ubuntu@sha256:1b3c82ca34c505c4951918b2e0a0c3db88cf266ebbf4196e4b0fba8fa137ada3

LABEL maintainer="mod_pagespeed developers"
LABEL description="Development environment for mod_pagespeed"

# Install clang-20 (abseil's absl_nonnull requires nullability_on_classes
# support; clang-18 lacks it, clang-19 partially supports it but misparses).
# Replace the default clang in /opt/llvm/bin so Bazel's CC=clang picks it up.
# apt retries transient fetch failures; the apt.llvm.org key is fetched with
# retries and only the key with the pinned fingerprint (the one
# https://apt.llvm.org/ publishes) goes into the keyring, so a failed fetch
# stops the build instead of leaving an empty key behind.
ARG LLVM_APT_KEY_FPR=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
RUN echo 'Acquire::Retries "5";' > /etc/apt/apt.conf.d/80-retries \
    && apt-get update && apt-get install -y --no-install-recommends \
    software-properties-common gnupg wget \
    && . /etc/os-release \
    && keytmp="$(mktemp -d)" \
    && for i in 1 2 3 4 5; do \
         wget -qO "${keytmp}/key.asc" --timeout=20 https://apt.llvm.org/llvm-snapshot.gpg.key \
           && [ -s "${keytmp}/key.asc" ] && break; \
         echo "apt.llvm.org key fetch attempt ${i} failed" >&2; sleep $((i * 5)); \
       done \
    && { [ -s "${keytmp}/key.asc" ] \
         || { echo "ERROR: could not fetch https://apt.llvm.org/llvm-snapshot.gpg.key" >&2; exit 1; }; } \
    && { GNUPGHOME="${keytmp}" gpg --batch --quiet --import "${keytmp}/key.asc"; \
         GNUPGHOME="${keytmp}" gpg --batch --with-colons --list-keys "${LLVM_APT_KEY_FPR}" 2>/dev/null \
           | awk -F: -v fpr="${LLVM_APT_KEY_FPR}" '$1=="fpr" && $10==fpr { ok=1 } END { exit !ok }' \
         || { echo "ERROR: the apt.llvm.org key file does not hold the pinned key ${LLVM_APT_KEY_FPR}" >&2; exit 1; }; } \
    && GNUPGHOME="${keytmp}" gpg --batch --export "${LLVM_APT_KEY_FPR}" > /usr/share/keyrings/llvm-snapshot.gpg \
    && rm -rf "${keytmp}" \
    && [ -s /usr/share/keyrings/llvm-snapshot.gpg ] \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-20 main" \
       > /etc/apt/sources.list.d/llvm-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
    clang-20 clang-format-20 clang-tidy-20 libc++-20-dev libc++abi-20-dev lld-20 \
    # libclang-rt-20-dev provides sanitizer runtime libraries (ASan, TSan, UBSan)
    # and the sanitizer headers (sanitizer/common_interface_defs.h etc.)
    libclang-rt-20-dev \
    && ln -sf /usr/bin/clang-20 /opt/llvm/bin/clang \
    && ln -sf /usr/bin/clang++-20 /opt/llvm/bin/clang++ \
    && ln -sf /usr/bin/clang-20 /opt/llvm/bin/clang-20 \
    && ln -sf /usr/bin/clang++-20 /opt/llvm/bin/clang++-20 \
    && ln -sf /usr/bin/lld-20 /opt/llvm/bin/lld \
    # The clang configs link with -fuse-ld=lld, and the clang driver then looks
    # for `ld.lld` next to clang itself: Bazel compiles with
    # -no-canonical-prefixes, so a symlinked clang resolves tools in the
    # symlink's directory, not its real install dir. /usr/bin carries only the
    # versioned name (ld.lld-20), so link ld.lld here explicitly; the base
    # image's own `ld.lld -> lld` relative symlink is not something to rely on.
    && ln -sf /usr/bin/ld.lld-20 /opt/llvm/bin/ld.lld \
    && ln -sf /usr/bin/clang-format-20 /opt/llvm/bin/clang-format \
    && ln -sf /usr/bin/clang-tidy-20 /opt/llvm/bin/clang-tidy \
    && ln -sf /usr/bin/run-clang-tidy-20 /opt/llvm/bin/run-clang-tidy \
    && rm -rf /var/lib/apt/lists/*

# Bazelisk 1.19.0 or newer: the base image's /usr/local/bin/bazel is Bazelisk
# 1.18.0, which finds the workspace root (and so .bazelversion) only through a
# WORKSPACE file. Without one it runs the latest Bazel instead of the pinned
# version; 1.19.0 and later also recognise MODULE.bazel and REPO.bazel. Same
# version and checksums as install/nginx/build_module_in_container.sh; bump
# them together.
ENV BAZELISK_VERSION=1.29.0
ENV BAZELISK_SHA256_AMD64=5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992
ENV BAZELISK_SHA256_ARM64=e20e8b0f4f240091b7a55bf17b9398bd4f40ee70ae0208dff95dd4c445fb4010
RUN arch="$(dpkg --print-architecture)" \
    && case "${arch}" in \
         amd64) sha="${BAZELISK_SHA256_AMD64}" ;; \
         arm64) sha="${BAZELISK_SHA256_ARM64}" ;; \
         *) echo "unsupported architecture ${arch}" >&2; exit 1 ;; \
       esac \
    && curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v${BAZELISK_VERSION}/bazelisk-linux-${arch}" -o /tmp/bazelisk \
    && echo "${sha}  /tmp/bazelisk" | sha256sum -c - \
    && install -m 0755 /tmp/bazelisk /usr/local/bin/bazel \
    && rm /tmp/bazelisk

# Install additional dependencies needed for mod_pagespeed
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Apache server and development headers (for mod_instaweb and system tests)
    apache2 \
    apache2-dev \
    # flex and bison for building libmemcached from source
    flex \
    bison \
    # AV1 stack (libaom/dav1d/libavif): nasm for x86_64 SIMD --
    # bazel/libaom.bzl sets ENABLE_NASM=1, which hard-fails configure without
    # nasm; meson + ninja for dav1d (Meson-native, see bazel/libdav1d.bzl)
    nasm \
    meson \
    ninja-build \
    # Redis tools for testing
    redis-tools \
    # Required for some builds
    gperf \
    uuid-dev \
    # Python 3 for libpsl psl-make-dafsa (PSL table generator) and system tests
    python3-pip \
    python3-distutils \
    # Java for closure compiler
    default-jre-headless \
    # nginx server and configure dependencies (for system tests and auto config headers)
    nginx \
    libpcre3-dev \
    zlib1g-dev \
    libssl-dev \
    # Useful development tools
    vim \
    less \
    && rm -rf /var/lib/apt/lists/*

# Packaging tools for building deb and rpm packages
RUN apt-get update && apt-get install -y --no-install-recommends \
    dpkg-dev fakeroot devscripts rpm \
    # selinux-policy-dev provides /usr/share/selinux/devel/Makefile, which the
    # serving-pair rpm build needs to compile the daemon's SELinux policy
    # module (install/rpm/build.sh).
    selinux-policy-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Node.js 20 LTS (required for npx google-closure-compiler)
# Manual APT source setup instead of pipe-to-shell (supply chain hardening)
RUN curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/nodesource.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/nodesource.gpg] https://deb.nodesource.com/node_20.x nodistro main" \
        > /etc/apt/sources.list.d/nodesource.list \
    && apt-get update && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

# Pre-install google-closure-compiler globally to avoid npx race conditions
# when multiple bazel genrules try to install it in parallel
RUN npm install -g google-closure-compiler@20260317.0.0

# ARM64: closure-compiler native binary needs glibc 2.32+ but the base image
# has 2.31. Remove native binary so npm falls back to Java-based compiler.
# Also install Temurin JRE 21 (closure-compiler Java needs Java 21+ on ARM64).
RUN set -e; if [ "$(dpkg --print-architecture)" = "arm64" ]; then \
      rm -f /usr/lib/node_modules/google-closure-compiler/node_modules/google-closure-compiler-linux-arm64/compiler 2>/dev/null || true \
      && curl -fsSL https://packages.adoptium.net/artifactory/api/gpg/key/public \
        | gpg --dearmor -o /usr/share/keyrings/adoptium-archive.gpg \
      && . /etc/os-release && echo "deb [signed-by=/usr/share/keyrings/adoptium-archive.gpg] https://packages.adoptium.net/artifactory/deb ${VERSION_CODENAME} main" \
        > /etc/apt/sources.list.d/adoptium.list \
      && apt-get update -qq && apt-get install -y -qq --no-install-recommends temurin-21-jre \
      && rm -rf /var/lib/apt/lists/*; \
    fi

# Install Python packages for system tests
RUN pip3 install --no-cache-dir --break-system-packages requests==2.32.4 pytest==8.3.5 2>/dev/null || \
    pip3 install --no-cache-dir requests==2.32.4 pytest==8.3.5

# Download and configure nginx source (required for building ngx_pagespeed)
# The nginx_repository bazel rule looks for source at /usr/local/src/nginx
ENV NGINX_VERSION=1.30.6
ENV NGINX_SHA256=3cf46f5ed7504d460125866b7bea99fb357f31d887e2e63b35503310e9b8b6c8
RUN curl -fsSL "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" -o /tmp/nginx.tar.gz \
    && echo "${NGINX_SHA256}  /tmp/nginx.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/nginx.tar.gz -C /usr/local/src \
    && rm /tmp/nginx.tar.gz \
    && mv "/usr/local/src/nginx-${NGINX_VERSION}" /usr/local/src/nginx \
    && cd /usr/local/src/nginx \
    && ./configure --with-cc=/opt/llvm/bin/clang \
        --with-http_v2_module --with-http_ssl_module --with-threads --with-compat \
    && make -j$(nproc)

# Create non-root user (bazel's hermetic Python requires non-root)
# Use fixed UID/GID that won't conflict with common system IDs
# Passwordless sudo so system tests can run as dev (not root) while still
# using sudo for apachectl, chown, etc. — avoids root-owned files in
# shared Bazel disk cache.
RUN apt-get update && apt-get install -y --no-install-recommends sudo \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -g 1000 dev \
    && useradd -m -u 1000 -g 1000 -s /bin/bash dev \
    && echo 'dev ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/dev

# Create directories and set ownership
RUN mkdir -p /bazel-cache /home/dev/.cache/bazel && \
    chown -R dev:dev /bazel-cache /home/dev/.cache

# Copy dev bashrc for convenient aliases
COPY dev.bashrc /etc/profile.d/dev.sh

# Set up bazel cache directory
ENV BAZEL_CACHE_DIR=/bazel-cache

# Set working directory to source mount point
WORKDIR /src

# Switch to non-root user
USER dev

# Default shell for interactive use
CMD ["/bin/bash", "-l"]
