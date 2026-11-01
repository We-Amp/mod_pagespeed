#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the shared-cache check's local-package mode
# (test/system/shared_cache): with PACKAGE_DIR the check installs the packages
# it is given and nothing from the public package repository, refuses a
# directory that does not hold the module package and the pinned optimizer's
# package (no fall back to the repository), and without PACKAGE_DIR it still
# installs the published pair exactly as before.  Also covers the pinned
# optimizer fetch (SHA256SUMS against the pin, package against SHA256SUMS).
# docker and the network are stubbed; nothing is built or started.
#
# SHARED_CACHE_DIR overrides the directory under test (used to prove the
# test fails against a broken copy).
set -uo pipefail
[ "${BASH_VERSINFO[0]}" -ge 4 ] || { echo "needs bash 4 or newer (run.sh does)" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SC="${SHARED_CACHE_DIR:-${ROOT}/test/system/shared_cache}"
T="$(mktemp -d)"
trap 'rm -rf "${T}"' EXIT
fail=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1" >&2; fail=1; }

# A pin for a made-up release, and a fake release directory served by file://.
VER=9.9.9
SUMDIR="${T}/rel/optimizer-v${VER}"
mkdir -p "${SUMDIR}" "${T}/bin"
printf 'optimizer package\n' > "${SUMDIR}/pagespeed-optimizer_${VER}_amd64.deb"
( cd "${SUMDIR}" && { command -v sha256sum >/dev/null && sha256sum pagespeed-optimizer_${VER}_amd64.deb || shasum -a 256 pagespeed-optimizer_${VER}_amd64.deb; } > SHA256SUMS )
sums_hash() { { command -v sha256sum >/dev/null && sha256sum "$1" || shasum -a 256 "$1"; } | cut -d' ' -f1; }
write_pin() { printf '{"tag": "optimizer-v%s", "sha256sums_sha256": "%s"}\n' "${VER}" "$1" > "${T}/pin.json"; }
write_pin "$(sums_hash "${SUMDIR}/SHA256SUMS")"

# docker stub: records every call; a build records its arguments and what the
# build context holds; starting the origin fails, which ends run.sh right after
# the build (the part under test).
cat > "${T}/bin/docker" <<'STUB'
#!/bin/bash
echo "$*" >> "${STUB_LOG}"
case "$1" in
  build)
    ctx="${@: -1}"
    echo "CONTEXT $(cd "$ctx/pkgs" && ls | tr '\n' ' ')" >> "${STUB_LOG}"
    ;;
  run) exit 1 ;;
esac
exit 0
STUB
chmod +x "${T}/bin/docker"

pkgs() { # dir module-package-name [optimizer version]
  mkdir -p "$1"
  : > "$1/$2_${3:-1.0}_amd64.deb"
  : > "$1/pagespeed-optimizer_${4:-${VER}}_amd64.deb"
}
run() { # server [extra env...] -- args; leaves rc in $rc and the call log in $LOG
  LOG="${T}/log.$RANDOM"
  : > "${LOG}"
  env PATH="${T}/bin:${PATH}" STUB_LOG="${LOG}" OPTIMIZER_PIN="${T}/pin.json" "$@" >"${LOG}.out" 2>&1
  rc=$?
}

# 1. Local mode builds from the staged packages and says so.
pkgs "${T}/a" mod-pagespeed
run PACKAGE_DIR="${T}/a" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && grep -q -- '--build-arg PACKAGES=local' "${LOG}" \
   && grep -q "^CONTEXT mod-pagespeed_1.0_amd64.deb pagespeed-optimizer_${VER}_amd64.deb $" "${LOG}"; then
  ok "local mode builds from the staged module and optimizer packages"
else bad "local mode builds from the staged packages (rc=${rc}): $(cat "${LOG}" "${LOG}.out")"; fi

pkgs "${T}/n" nginx-module-pagespeed
run PACKAGE_DIR="${T}/n" bash "${SC}/run.sh" nginx
if grep -q -- '--build-arg PACKAGES=local' "${LOG}" && grep -q "^CONTEXT nginx-module-pagespeed_1.0_amd64.deb pagespeed-optimizer_${VER}_amd64.deb $" "${LOG}"; then
  ok "local mode, nginx"
else bad "local mode, nginx: $(cat "${LOG}")"; fi

# 2. Without PACKAGE_DIR: the published pair, empty package directory, and the
# fresh library still mounted over the packaged one.
: > "${T}/m.so"
run bash "${SC}/run.sh" apache "${T}/m.so"
if grep -q -- '--build-arg PACKAGES=published' "${LOG}" && grep -q '^CONTEXT $' "${LOG}" \
   && grep -q -- 'MODULE_SO=/module.so' "${LOG}"; then
  ok "published mode is unchanged (published packages, library swapped in)"
else bad "published mode unchanged: $(cat "${LOG}")"; fi

# 3. A directory without the optimizer, or with the wrong pair, is an error and
# never a build from the repository.
mkdir -p "${T}/b"; : > "${T}/b/mod-pagespeed_1.0_amd64.deb"
run PACKAGE_DIR="${T}/b" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR' "${LOG}.out"; then ok "no optimizer package: refused, no build"
else bad "no optimizer package must be refused without a build (rc=${rc}): $(cat "${LOG}")"; fi

pkgs "${T}/c" mod-pagespeed 1.0 8.8.8
run PACKAGE_DIR="${T}/c" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR' "${LOG}.out"; then ok "optimizer package of another version: refused, no build"
else bad "wrong optimizer version must be refused without a build (rc=${rc}): $(cat "${LOG}")"; fi

mkdir -p "${T}/d"; : > "${T}/d/pagespeed-optimizer_${VER}_amd64.deb"
run PACKAGE_DIR="${T}/d" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR' "${LOG}.out"; then ok "no module package: refused, no build"
else bad "no module package must be refused without a build (rc=${rc}): $(cat "${LOG}")"; fi

run PACKAGE_DIR="${T}/nonexistent" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR' "${LOG}.out"; then ok "missing directory: refused, no build"
else bad "missing directory must be refused without a build (rc=${rc})"; fi

run PACKAGE_DIR="${T}/a" bash "${SC}/run.sh" apache "${T}/m.so"
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR' "${LOG}.out"; then ok "PACKAGE_DIR with a module.so: refused"
else bad "PACKAGE_DIR with a module.so must be refused (rc=${rc})"; fi

# 4. The Dockerfiles keep the published install, and the local branch does not
# touch the repository.
for f in apache:mod-pagespeed nginx:nginx-module-pagespeed; do
  df="${SC}/Dockerfile.${f%%:*}"; pk="${f#*:}"
  flat="$(tr -d '\\\n' < "${df}" | tr -s ' ')"
  local_branch="$(sed -n 's/.*if \[ "\$PACKAGES" = local \]; then \(.*\); else .*/\1/p' <<< "${flat}")"
  pub_branch="$(sed -n 's/.*; else \(.*\); fi$/\1/p' <<< "${flat}")"
  if grep -q 'apt-get install -y /pkgs/\*.deb' <<< "${local_branch}" && ! grep -q 'install.sh' <<< "${local_branch}"; then
    ok "Dockerfile.${f%%:*}: local branch installs /pkgs only"
  else bad "Dockerfile.${f%%:*}: local branch must install /pkgs/*.deb and not use the repository"; fi
  if grep -q 'curl -fsSL https://packages.modpagespeed.com/install.sh | sh' <<< "${pub_branch}" \
     && grep -q "\"${pk}=\${PAIR_VERSION}-\*\" \"pagespeed-optimizer=\${PAIR_VERSION}\"" <<< "${pub_branch}"; then
    ok "Dockerfile.${f%%:*}: published branch installs the pinned pair from the repository"
  else bad "Dockerfile.${f%%:*}: published branch must install ${pk} and pagespeed-optimizer at the pair version from the repository"; fi
  grep -q '^ARG PACKAGES=published$' "${df}" && ok "Dockerfile.${f%%:*}: defaults to published" \
    || bad "Dockerfile.${f%%:*}: PACKAGES must default to published"
done

# 5. The optimizer fetch.
export OPTIMIZER_PIN="${T}/pin.json" OPTIMIZER_RELEASE_BASE="file://${T}/rel"
if bash "${SC}/fetch-optimizer-deb.sh" "${T}/o1" amd64 >/dev/null 2>&1 && [ -f "${T}/o1/pagespeed-optimizer_${VER}_amd64.deb" ]; then
  ok "fetch: verified package is fetched"
else bad "fetch: a package matching the pin and the list must be fetched"; fi

write_pin "0000000000000000000000000000000000000000000000000000000000000000"
if ! bash "${SC}/fetch-optimizer-deb.sh" "${T}/o2" amd64 >/dev/null 2>&1 && [ ! -e "${T}/o2/pagespeed-optimizer_${VER}_amd64.deb" ]; then
  ok "fetch: SHA256SUMS that does not match the pin is refused"
else bad "fetch: SHA256SUMS that does not match the pin must be refused"; fi

write_pin "$(sums_hash "${SUMDIR}/SHA256SUMS")"
printf 'tampered\n' > "${SUMDIR}/pagespeed-optimizer_${VER}_amd64.deb"
if ! bash "${SC}/fetch-optimizer-deb.sh" "${T}/o3" amd64 >/dev/null 2>&1 && [ ! -e "${T}/o3/pagespeed-optimizer_${VER}_amd64.deb" ]; then
  ok "fetch: a package that does not match the list is refused"
else bad "fetch: a package that does not match the verified list must be refused"; fi

exit "${fail}"
