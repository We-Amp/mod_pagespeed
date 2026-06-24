# Docker development environment for mod_pagespeed
# Based on Envoy's build image which includes clang, bazel, and most dependencies

# Tag: 86873047235e9b8232df989a5999b9bebf9db69c (pinned by digest for immutability)
FROM envoyproxy/envoy-build-ubuntu@sha256:1b3c82ca34c505c4951918b2e0a0c3db88cf266ebbf4196e4b0fba8fa137ada3

LABEL maintainer="mod_pagespeed developers"
LABEL description="Development environment for mod_pagespeed"

# Install clang-20 (abseil's absl_nonnull requires nullability_on_classes
# support; clang-18 lacks it, clang-19 partially supports it but misparses).
# Replace the default clang in /opt/llvm/bin so Bazel's CC=clang picks it up.
RUN apt-get update && apt-get install -y --no-install-recommends \
    software-properties-common gnupg wget \
    && . /etc/os-release \
    && wget -qO /usr/share/keyrings/llvm-snapshot.gpg.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-20 main" \
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
    && ln -sf /usr/bin/clang-format-20 /opt/llvm/bin/clang-format \
    && ln -sf /usr/bin/clang-tidy-20 /opt/llvm/bin/clang-tidy \
    && ln -sf /usr/bin/run-clang-tidy-20 /opt/llvm/bin/run-clang-tidy \
    && rm -rf /var/lib/apt/lists/*

# Install additional dependencies needed for mod_pagespeed
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Apache server and development headers (for mod_instaweb and system tests)
    apache2 \
    apache2-dev \
    # flex and bison for building libmemcached from source
    flex \
    bison \
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
ENV NGINX_VERSION=1.30.2
ENV NGINX_SHA256=7df3090907fca3cc0e456d6dc00ceb230da74ea88026ceff0affc29dbbd9ac4c
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
