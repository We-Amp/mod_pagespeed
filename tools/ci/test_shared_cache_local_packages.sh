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
# SHARED_CACHE_DIR and WORKFLOWS_DIR override the directories under test
# (used to prove the test fails against a broken copy).
set -uo pipefail
[ "${BASH_VERSINFO[0]}" -ge 4 ] || { echo "needs bash 4 or newer (run.sh does)" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SC="${SHARED_CACHE_DIR:-${ROOT}/test/system/shared_cache}"
WF="${WORKFLOWS_DIR:-${ROOT}/.github/workflows}"
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
# build context holds; starting the first cache fails, which ends run.sh right
# after the origin is up (the part under test).
cat > "${T}/bin/docker" <<'STUB'
#!/bin/bash
echo "$*" >> "${STUB_LOG}"
case "$1" in
  build)
    ctx="${@: -1}"
    echo "CONTEXT $(cd "$ctx/pkgs" && ls | tr '\n' ' ')" >> "${STUB_LOG}"
    ;;
  run)
    # The first cache container ends run.sh right after the origin is up.
    case "$*" in *nginx:1.27-alpine*) exit 1 ;; esac
    ;;
  exec)
    # run.sh's check for a package repository source in the image.
    case "$*" in *"/etc/apt"*) exit "${STUB_APT_RC:-1}" ;; esac
    ;;
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
   && grep -q "^CONTEXT mod-pagespeed_1.0_amd64.deb pagespeed-optimizer_${VER}_amd64.deb $" "${LOG}" \
   && grep -q '^exec .* grep .*modpagespeed /etc/apt' "${LOG}"; then
  ok "local mode builds from the staged module and optimizer packages and checks the image for a repository source"
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

# A repository source in the image fails a local run before any cache starts.
run STUB_APT_RC=0 PACKAGE_DIR="${T}/a" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 1 ] && grep -q 'modpagespeed package source' "${LOG}.out" && ! grep -q 'nginx:1.27-alpine' "${LOG}"; then
  ok "local mode fails when the image has a modpagespeed package source"
else bad "local mode must fail on a modpagespeed package source before the caches start (rc=${rc}): $(cat "${LOG}.out")"; fi
# Published mode adds that source itself and never looks for it.
run STUB_APT_RC=0 bash "${SC}/run.sh" apache "${T}/m.so"
if ! grep -q '/etc/apt' "${LOG}"; then ok "published mode does not look for a repository source"
else bad "published mode must not look for a repository source"; fi

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

pkgs "${T}/e" mod-pagespeed 1.0
: > "${T}/e/mod-pagespeed_1.1_amd64.deb"
run PACKAGE_DIR="${T}/e" bash "${SC}/run.sh" apache
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'exactly one' "${LOG}.out"; then ok "two module packages: refused, no build"
else bad "two module packages must be refused without a build (rc=${rc}): $(cat "${LOG}" "${LOG}.out")"; fi

run LOCAL_PACKAGES=true bash "${SC}/run.sh" apache "${T}/m.so"
if [ "${rc}" -eq 2 ] && ! grep -q '^build' "${LOG}" && grep -q 'PACKAGE_DIR is empty' "${LOG}.out"; then ok "LOCAL_PACKAGES=true without PACKAGE_DIR: refused, no build"
else bad "LOCAL_PACKAGES=true without PACKAGE_DIR must be refused without a build (rc=${rc}): $(cat "${LOG}" "${LOG}.out")"; fi

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
  local_branch="$(sed -e 's/^ *//' -e 's/ *$//' <<< "${local_branch}")"
  if [ "${local_branch}" = 'apt-get install -y /pkgs/*.deb' ]; then
    ok "Dockerfile.${f%%:*}: local branch is exactly: apt-get install -y /pkgs/*.deb"
  else bad "Dockerfile.${f%%:*}: local branch must be exactly 'apt-get install -y /pkgs/*.deb', got '${local_branch}'"; fi
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

write_pin "$(sums_hash "${SUMDIR}/SHA256SUMS")"
mkdir -p "${T}/rel2/optimizer-v${VER}"
cp "${SUMDIR}/pagespeed-optimizer_${VER}_amd64.deb" "${T}/rel2/optimizer-v${VER}/"
printf '%s  some-other-file.deb\n' "$(sums_hash "${SUMDIR}/SHA256SUMS")" > "${T}/rel2/optimizer-v${VER}/SHA256SUMS"
write_pin "$(sums_hash "${T}/rel2/optimizer-v${VER}/SHA256SUMS")"
if OPTIMIZER_RELEASE_BASE="file://${T}/rel2" bash "${SC}/fetch-optimizer-deb.sh" "${T}/o4" amd64 >"${T}/o4.out" 2>&1; then
  bad "fetch: a verified list that does not name the package must be refused"
elif grep -q 'does not name' "${T}/o4.out" && [ ! -e "${T}/o4/pagespeed-optimizer_${VER}_amd64.deb" ]; then
  ok "fetch: a verified list that does not name the package is refused"
else bad "fetch: refusal for a list without the package must say so: $(cat "${T}/o4.out")"; fi

# 6. Workflow wiring: the scheduled night and a plain dispatch stay on the
# published packages; only an explicit local dispatch switches, and says so.
NIGHTLY="${WF}/nightly.yml"; PIPE="${WF}/pipeline-nightly.yml"
count() { grep -cF -- "$1" "$2" || true; }
[ "$(count "      shared_cache_packages: \${{ inputs.shared_cache_packages || 'published' }}" "${NIGHTLY}")" = 1 ] \
  && ok "nightly.yml passes the input with a published fallback (schedule and plain dispatch)" \
  || bad "nightly.yml must pass shared_cache_packages: \${{ inputs.shared_cache_packages || 'published' }}"
nd="$(awk '/^      shared_cache_packages:/{f=1;next} f&&/default:/{print $2;exit} f&&/^      [a-z_]+:/{exit}' "${NIGHTLY}")"
[ "${nd}" = published ] && ok "nightly.yml dispatch input defaults to published" || bad "nightly.yml dispatch input must default to published (got '${nd}')"
pd="$(awk '/^      shared_cache_packages:/{f=1;next} f&&/default:/{print $2;exit} f&&/^      [a-z_]+:/{exit}' "${PIPE}")"
[ "${pd}" = '"published"' ] && ok "pipeline-nightly.yml input defaults to published" || bad "pipeline-nightly.yml input must default to \"published\" (got '${pd}')"
[ "$(count "      LOCAL_PACKAGES: \${{ inputs.shared_cache_packages == 'local' && 'true' || 'false' }}" "${PIPE}")" = 1 ] \
  && ok "LOCAL_PACKAGES is true only for an explicit local input" \
  || bad "pipeline-nightly.yml must set LOCAL_PACKAGES: \${{ inputs.shared_cache_packages == 'local' && 'true' || 'false' }}"
[ "$(grep -c "LOCAL_PACKAGES:" "${PIPE}")" = 1 ] && ok "LOCAL_PACKAGES is set in one place" || bad "LOCAL_PACKAGES must be set in exactly one place"
[ "$(grep -c "'local'" "${NIGHTLY}")" = 1 ] && ok "nightly.yml names local only in the run name" || bad "nightly.yml may name 'local' only in the run name"
RUN_NAME="run-name: \${{ github.event_name == 'workflow_dispatch' && inputs.shared_cache_packages == 'local' && 'Nightly (local packages)' || github.event_name == 'workflow_dispatch' && inputs.full_tier && 'Nightly (full)' || 'Nightly' }}"
grep -qFx -- "${RUN_NAME}" "${NIGHTLY}" \
  && ok "the run name is exactly: local first, then full, then plain" || bad "nightly.yml run-name must be exactly: ${RUN_NAME}"
# The seam: the check steps hand the staged directory to run.sh, the module
# package depends on the pinned pair, and the fetch runs only in local mode.
[ "$(count '            export PACKAGE_DIR="${RUNNER_TEMP}/shared-cache/pkgs-apache"' "${PIPE}")" = 1 ] \
  && ok "the Apache check exports PACKAGE_DIR" || bad "the Apache check step must export PACKAGE_DIR=\${RUNNER_TEMP}/shared-cache/pkgs-apache"
[ "$(count '            export PACKAGE_DIR="${RUNNER_TEMP}/shared-cache/pkgs-nginx"' "${PIPE}")" = 1 ] \
  && ok "the nginx check exports PACKAGE_DIR" || bad "the nginx check step must export PACKAGE_DIR=\${RUNNER_TEMP}/shared-cache/pkgs-nginx"
[ "$(count 'install/debian/build.sh -a x64 -b . -o /out/pkgs-apache -d "${PAIR_PKG_VERSION}"' "${PIPE}")" = 1 ] \
  && ok "the Apache package is built with the dependency on the pinned pair (-d)" || bad "the Apache package must be built with -d \"\${PAIR_PKG_VERSION}\""
grep -A2 'name: Fetch the pinned optimizer package (local packages)' "${PIPE}" | grep -qF "if: env.LOCAL_PACKAGES == 'true'" \
  && ok "the optimizer fetch runs only in local mode" || bad "the optimizer fetch step must carry if: env.LOCAL_PACKAGES == 'true'"
[ "$(grep -cF 'if [ "${LOCAL_PACKAGES}" = true ]; then' "${PIPE}")" -ge 4 ] \
  && ok "package builds and check steps are guarded by LOCAL_PACKAGES" || bad "the package builds and both check steps must be guarded by LOCAL_PACKAGES"
grep -A8 'name: Record the package mode' "${PIPE}" | grep -q 'GITHUB_STEP_SUMMARY' \
  && ok "the shared-cache job writes its package mode to the job summary" || bad "the shared-cache job must write its package mode to GITHUB_STEP_SUMMARY"

exit "${fail}"
