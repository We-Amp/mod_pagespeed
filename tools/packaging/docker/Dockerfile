# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Packaging image for the optimizer release lane: dpkg-deb (in the base),
# rpmbuild, readelf and cpio, on a digest-pinned base so the release lane has
# no floating network dependency at package time. cpio is not needed to build
# a package; it is what the build scripts' --self-test reads a built payload
# back with, and CI runs those self-tests in THIS image on every PR. The
# multi-arch index digest resolves per-platform (amd64 and arm64 both build
# from it). python3 is there for the same reason as cpio: the
# self-tests read a built payload back and parse the shipped container seccomp
# profile as JSON, and this base carries no python at all. It has to be the
# full python3, not python3-minimal: the minimal package omits the json
# module, so the self-test fails with ModuleNotFoundError rather than a
# packaging verdict.
# Bump deliberately; the workflow content-hashes this directory into the
# image tag, so an edit here rebuilds the image on next use.

# The EL sysusers rpm macros. rpm on a Debian-family base ships an EMPTY
# /usr/lib/rpm/macros.d, so %sysusers_requires_compat and
# %sysusers_create_compat are undefined there and a spec that names them
# unconditionally fails to parse. The spec this image builds carries its own
# fallback, but the release lane should emit the SAME scriptlet an EL builder
# emits, so take the macros from the package that defines them rather than
# reimplementing them. Two files, both self-contained: the macro definitions
# and the bash generator they shell out to.
FROM almalinux:9@sha256:245f8aab832fe800aa42bdab5fad75ebbd8e0b4c9f24820a83c638b177857ae2 AS elmacros
RUN dnf -y install --setopt=install_weak_deps=False systemd-rpm-macros \
    && dnf clean all

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
ENV DEBIAN_FRONTEND=noninteractive
# apt retries each file and times out a stalled connection; apt-get update
# exits 0 when an index fails to download, so a failed index is caught here
# and the update is retried once instead of failing later with "Unable to
# locate package". The retry config is removed again, so the image contents
# do not change.
RUN printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' \
        > /etc/apt/apt.conf.d/80-ci-retries \
    && for attempt in 1 2; do \
         out="$(apt-get -qq update 2>&1)" \
           && ! printf '%s\n' "$out" | grep -qE '^(W|E): (Failed to fetch|Some index files failed)' \
           && break; \
         printf '%s\n' "$out" | tail -n 12 >&2; \
         [ "$attempt" -eq 2 ] && { echo "apt-get update failed twice" >&2; exit 1; }; \
         sleep 20; \
       done \
    && apt-get -qq -y install --no-install-recommends rpm binutils cpio python3 \
    && rm -rf /var/lib/apt/lists/* /etc/apt/apt.conf.d/80-ci-retries
COPY --from=elmacros /usr/lib/rpm/macros.d/macros.sysusers /usr/lib/rpm/macros.d/macros.sysusers
COPY --from=elmacros /usr/lib/rpm/sysusers.generate-pre.sh /usr/lib/rpm/sysusers.generate-pre.sh
