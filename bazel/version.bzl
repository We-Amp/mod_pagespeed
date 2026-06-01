# Copyright 2024 The mod_pagespeed Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Build rule to generate version.h from version.h.in template."""

def version_header(
        name,
        template,
        version_file,
        out,
        company_fullname = "We-Amp",
        company_shortname = "We-Amp",
        product_fullname = "mod_pagespeed",
        product_shortname = "modpagespeed",
        copyright = "Copyright 2010-2026 We-Amp",
        official_build = "0"):
    """Generate version.h from template with version substitution.

    Args:
        name: Rule name
        template: The version.h.in template file
        version_file: The VERSION file containing MAJOR, MINOR, BUILD, PATCH, PRERELEASE
        out: Output filename (version.h)
        company_fullname: Company full name string
        company_shortname: Company short name string
        product_fullname: Product full name string
        product_shortname: Product short name string
        copyright: Copyright string
        official_build: Official build flag ("0" or "1")
    """

    cmd = """
        # Read version values from VERSION file
        . $(location {version_file})

        # Compute PRERELEASE_SUFFIX: "-beta.1" when set, "" when empty
        if [ -n "$$PRERELEASE" ]; then
            PRERELEASE_SUFFIX="-$$PRERELEASE"
        else
            PRERELEASE_SUFFIX=""
        fi

        # Derive numeric FILEVERSION_BUILD (4th component of Windows
        # VS_VERSION_INFO FILEVERSION) so MSI's component-version logic
        # orders releases correctly across betas. "beta.N" -> N. Empty
        # prerelease (final release) -> 65535 so finals sort higher than
        # any beta. Used by the .rc generated for pagespeed_iis.dll.
        if [ -n "$$PRERELEASE" ]; then
            FILEVERSION_BUILD=$$(echo "$$PRERELEASE" | grep -oE '[0-9]+' | head -1)
            FILEVERSION_BUILD=$${{FILEVERSION_BUILD:-0}}
            FILEVERSION_FLAGS="0x2L"  # VS_FF_PRERELEASE
        else
            FILEVERSION_BUILD=65535
            FILEVERSION_FLAGS="0x0L"
        fi

        # Resolve commit SHA. Priority chain:
        #   1. GIT_COMMIT file (if not "dev" and not empty)
        #   2. LASTCHANGE from VERSION file (backward compat for source tarballs)
        #   3. git rev-parse (local dev builds, needs local=True)
        #   4. "0" (final fallback)
        GIT_COMMIT_RAW=$$(cat $(location //:GIT_COMMIT) 2>/dev/null || echo "")
        if [ -n "$$GIT_COMMIT_RAW" ] && [ "$$GIT_COMMIT_RAW" != "dev" ]; then
            GIT_COMMIT_FULL="$$GIT_COMMIT_RAW"
            LASTCHANGE="$${{GIT_COMMIT_FULL:0:7}}"
        elif [ -n "$${{LASTCHANGE:-}}" ]; then
            GIT_COMMIT_FULL="$$LASTCHANGE"
        else
            GIT_COMMIT_FULL=$$(git rev-parse HEAD 2>/dev/null || echo "0")
            LASTCHANGE=$$(git rev-parse --short HEAD 2>/dev/null || echo "0")
        fi

        # Substitute all placeholders using sed
        sed -e "s/@MAJOR@/$$MAJOR/g" \
            -e "s/@MINOR@/$$MINOR/g" \
            -e "s/@BUILD@/$$BUILD/g" \
            -e "s/@PATCH@/$$PATCH/g" \
            -e "s/@PRERELEASE@/$${{PRERELEASE:-}}/g" \
            -e "s/@PRERELEASE_SUFFIX@/$$PRERELEASE_SUFFIX/g" \
            -e "s/@FILEVERSION_BUILD@/$$FILEVERSION_BUILD/g" \
            -e "s/@FILEVERSION_FLAGS@/$$FILEVERSION_FLAGS/g" \
            -e "s/@LASTCHANGE@/$$LASTCHANGE/g" \
            -e "s/@GIT_COMMIT_FULL@/$$GIT_COMMIT_FULL/g" \
            -e "s/@COMPANY_FULLNAME@/{company_fullname}/g" \
            -e "s/@COMPANY_SHORTNAME@/{company_shortname}/g" \
            -e "s/@PRODUCT_FULLNAME@/{product_fullname}/g" \
            -e "s/@PRODUCT_SHORTNAME@/{product_shortname}/g" \
            -e "s/@COPYRIGHT@/{copyright}/g" \
            -e "s/@OFFICIAL_BUILD@/{official_build}/g" \
            $(location {template}) > $@
    """.format(
        version_file = version_file,
        template = template,
        company_fullname = company_fullname,
        company_shortname = company_shortname,
        product_fullname = product_fullname,
        product_shortname = product_shortname,
        copyright = copyright,
        official_build = official_build,
    )

    native.genrule(
        name = name,
        srcs = [template, version_file, "//:GIT_COMMIT"],
        outs = [out],
        cmd = cmd,
        local = True,  # Needed for git rev-parse to access .git directory
        # Never consult or write the remote cache for this action.
        #
        # Why: the Windows release build for beta.12 produced a DLL stamped
        # with beta.10's version string even though the release workflow had
        # patched `net/instaweb/public/VERSION` to `PRERELEASE=beta.12` before
        # invoking bazel. Empirical reproduction on Linux/macOS with
        # `--disk_cache` only (no remote cache) showed Bazel correctly re-runs
        # this genrule on every VERSION content change, and correctly hits/
        # misses the disk cache when the same content recurs — so Bazel's
        # action-key computation is content-correct.
        #
        # The Windows release configuration is the only one that points at the
        # shared remote bazel-remote (`build:windows --remote_cache=...` in
        # pagespeed.bazelrc), and it is the only configuration that has ever
        # produced this stale-version symptom. That makes the remote cache the
        # only remaining suspect surface (Bazel client invalidates correctly).
        #
        # This action is microseconds-cheap (one shell pipeline writing one
        # header), so the remote cache buys nothing. Disabling remote cache
        # for this one action removes the only place a stale version.h could
        # come from without touching the rest of the build's cache behavior.
        # Local disk cache (L1) still works normally.
        tags = ["no-remote-cache"],
    )
