# mod_pagespeed 1.15 — cPanel EasyApache 4 packaging

Ships `ea-apache24-mod_pagespeed` for cPanel WHM operators on EL8 and EL9
(AlmaLinux / Rocky; CloudLinux caveat below).

`ea-apache24-mod_pagespeed` ships for **both EL8 and EL9** from
`packages.modpagespeed.com` (yum repo paths `ea4/el8` and `ea4/el9`;
EL8 shipped 2026-06-09 in v1.15.0+r7). The EL8 binary is built in an
AlmaLinux 8 container with gcc-toolset-13, statically linked libstdc++,
and a glibc floor ≤ 2.28, so it loads on stock EL8. CloudLinux 8 is
expected to work (AlmaLinux 8 compatible) but has not yet been validated
on a real CloudLinux host; that validation is a tracked follow-up.

This is a **repackager** spec: it does not compile the module from source.
Instead, it pulls the binary `mod-pagespeed-<version>.x86_64.rpm` we already
publish to `packages.modpagespeed.com` (built against generic Apache 2.4
via this repo's `release.yml`), extracts its files, and re-lays them under
EA4's `/etc/apache2/` + `/usr/lib64/apache2/` layout. Apache 2.4 module magic
number is the same on both targets (`20120211`), so the .so is ABI-compatible
without recompilation.

This approach is identical in shape to the archived
`apache/incubator-pagespeed-cpanel` spec (last touched 2016-06-27 by Prajith
Palakkuda), which extracted the .so from Google's prebuilt RPM. Our update
swaps the source URL to our maintained upstream and modernises the spec to
EA4 macro conventions documented in cPanel's `ea4-example-specs` repo.

## Layout

```
packaging/cpanel/
├── ea-apache24-mod_pagespeed.spec   RPM spec
├── pagespeed-cpanel.conf            LoadModule fragment (→ conf.modules.d/)
├── README.md                        this file
└── upstream/                        gitignored — reference materials
    ├── ea-apache24-mod_pagespeed-latest-stable.src.rpm
    ├── mod_pagespeed.spec           original 2016 spec
    └── 456_pagespeed.conf           original 2016 config
```

## Local build

Requires Docker (for the AlmaLinux 9 + EA4 build environment). For an el8
RPM, swap `almalinux:9` for `almalinux:8` below — the `.el8` dist tag follows
from the container OS. Run from this directory:

```bash
docker run --rm --platform linux/amd64 \
  -v "$PWD:/work" -w /work almalinux:9 bash -ec '
    dnf install -q -y rpm-build cpio createrepo_c >/dev/null

    # EA4 build deps — none are actually consumed at build time. This is a
    # repackager spec: it does not compile against `ea-apache24-devel` headers,
    # it only consumes EA4 macro expansions (apache_confdir, apache_libexecdir,
    # etc.). cPanel hosts the official EA4 yum repo at securedownloads.cpanel.net
    # but it is license-gated. The OBS mirror at
    # `isv:cpanel:dev:EA4` / `isv:cpanel:EA4` previously published the same
    # packages openly, but the AlmaLinux_9 target was discontinued or
    # restructured (the project root still resolves, but no distro subdirs are
    # published as of 2026-05).
    #
    # Workaround: inject the EA4 macro block into /etc/rpm/macros.apache2 and
    # build with `--nodeps`. This matches what release.yml's EA4 build cell
    # does in CI.
    cat > /etc/rpm/macros.apache2 <<MACROS
%apache_confdir       /etc/apache2/conf.d
%apache_modulesdir    /usr/lib64/apache2/modules
%apache_sysconfdir    /etc/apache2
%apache_libexecdir    /usr/lib64/apache2
MACROS

    mkdir -p ~/rpmbuild/{SOURCES,SPECS}
    cp pagespeed-cpanel.conf ~/rpmbuild/SOURCES/
    cp ea-apache24-mod_pagespeed.spec ~/rpmbuild/SPECS/
    spectool -g -R ~/rpmbuild/SPECS/ea-apache24-mod_pagespeed.spec

    # Binary-only with --nodeps. The repackager doesn't need ea-apache24-devel
    # at build time — see comment above. Drop --nodeps once a license-gated
    # cPanel build image is in CI rotation.
    rpmbuild -bb --nodeps ~/rpmbuild/SPECS/ea-apache24-mod_pagespeed.spec

    cp -r ~/rpmbuild/RPMS /work/build-out/
  '
```

After completion, `build-out/RPMS/x86_64/ea-apache24-mod_pagespeed-<ver>.x86_64.rpm`
holds the installable RPM. Verify with:

```bash
rpm -qpl  build-out/RPMS/x86_64/ea-apache24-mod_pagespeed-*.x86_64.rpm
rpm -qpR  build-out/RPMS/x86_64/ea-apache24-mod_pagespeed-*.x86_64.rpm
rpm --checksig build-out/RPMS/x86_64/ea-apache24-mod_pagespeed-*.x86_64.rpm   # unsigned at this stage
```

## Production publishing

In production, this spec is built by the CI matrix (`release.yml`'s EA4 build
cells, el8 + el9) using the same `/etc/rpm/macros.apache2` injection +
`rpmbuild --nodeps` path as the local build above — the repackager doesn't
consume any headers from `ea-apache24-devel`, only macro expansions, so a
license-gated cPanel build image is not required. If one becomes available in
CI rotation, drop `--nodeps` and install the real EA4 devel package via
`securedownloads.cpanel.net`. The resulting el8 and el9 RPMs (the `.elN` dist
tag is set by the build container's OS) get signed by the packages key and
published to `packages.modpagespeed.com/yum/ea4/el8/x86_64/Packages/` and
`packages.modpagespeed.com/yum/ea4/el9/x86_64/Packages/`.

See `docs/operations/packages-repo-runbook.md` for the publishing
flow.

## Customer install

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo yum install ea-apache24-mod_pagespeed
```

The module appears in WHM EA4 Customize → Apache Modules afterward.

Default cPanel installs ship `ea-apache24-mod_ruid2`. Our spec declares
`Conflicts: ea-apache24-mod_ruid2` (the two modules both rewrite the request
handler chain), so the operator must `dnf remove ea-apache24-mod_ruid2` or
unselect mod_ruid2 in the EA4 profile before installing mod_pagespeed.
This is intentional — pick one, not both.

## Licensing for hosts

Licensing is per site (registrable domain), not per server or instance.
A hosting provider running many customer sites on one host should use the
Hoster tier: a per-host monthly plan covering all sites on that host; see the public licensing terms.
Hoster setup is manual; contact https://modpagespeed.com/contact/.
The full ladder is at https://modpagespeed.com/pricing/ and license terms
at https://we-amp.com/licensing/.
