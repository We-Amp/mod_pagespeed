%global mps_version 1.15.0
%global mps_release 1
%global upstream_rpm mod-pagespeed-%{mps_version}-%{mps_release}.x86_64.rpm
# The stock upstream RPM filename carries NO dist tag (it is the same artifact
# regardless of which glibc floor it was built against). The EA4 build cell
# (release.yml) PRE-PLACES the matching stock RPM into rpmbuild's SOURCES/ from
# the per-floor CI artifact (x64 for el9, x64-el8 for el8), so Source1's URL is
# only a documented fallback for manual rebuilds. It is parameterized by
# %{upstream_dist} (default el9) so `rpmbuild --define "upstream_dist el8"`
# points at the el8 stock tree if one is published. The OUTPUT rpm's .elN dist
# tag comes independently from %{?dist} (set by the build container's OS) — see
# the el8/el9 ea4-build matrix in release.yml.
%global upstream_dist el9
%global upstream_url https://packages.modpagespeed.com/yum/%{upstream_dist}/x86_64/Packages/%{upstream_rpm}

# Repackager spec — we don't build from source, so no debug subpackage to
# auto-generate. Default RHEL/AlmaLinux 9 macros enable -debugsource which
# expects compiled source files; without this override, %%files for the
# debug subpackage is empty and rpmbuild aborts.
%global debug_package %{nil}

# Suppress /usr/lib/.build-id/ symlinks. Without a -debuginfo subpackage
# they would dangle (no debuginfo to point at), so they're worse than nothing.
%global _build_id_links none

# Suppress auto-requires generation for the bundled .so — we declare its
# dependencies explicitly below. Without this, RPM would resolve a generic
# `httpd >= 2.4` Requires from the upstream binary's metadata, which doesn't
# match EA4's `ea-apache24` virtual provides.
AutoReq: no

Name:    ea-apache24-mod_pagespeed
# Epoch ranks ABOVE Version in RPM/dnf ordering — a higher epoch always wins
# regardless of the version string. cPanel's own EA4 repo (EA4-c9) ships an
# `ea-apache24-mod_pagespeed` built at Epoch=1 (their 1:1.13.35.2). With no
# epoch (=0) on our package, `0:1.15.0` LOSES to cPanel's `1:1.13.35.2`, so a
# plain `dnf install ea-apache24-mod_pagespeed` and the WHM EasyApache 4
# "Customize" checkbox would silently install cPanel's stale 1.13.35.2 instead
# of ours. Verified on a real cPanel rig 2026-06-01 (the epoch is in cPanel's
# actual RPM header, not just repodata). Epoch:2 puts us above cPanel's 1 so
# ours wins the default resolution. Never lower this; only raise it if cPanel
# ever ships an epoch >= 2. See corp the design record (Epoch amendment) + the memory
# note reference_ea4_epoch_conflict.
Epoch:   2
Version: %{mps_version}
Release: %{mps_release}%{?dist}.cpanel
Summary: mod_pagespeed 1.15 for cPanel EasyApache 4
License: Apache-2.0
URL:     https://modpagespeed.com/

Source0: pagespeed-cpanel.conf
Source1: %{upstream_url}

BuildArch:     x86_64
BuildRequires: cpio
BuildRequires: ea-apache24-devel

Requires(pre): ea-apache24
Requires:      ea-apache24
Requires:      ea-apache24-mmn = %{_httpd_mmn}
# ea-apache24-mod_version is a separate subpackage of ea-apache24 in cPanel's
# EA4 repo and is NOT installed by default on a stock cPanel host. Without it
# the LoadModule fragment in pagespeed-cpanel.conf falls through to load
# mod_version.so from a path that does not exist, and httpd refuses to start.
# Surfaced by the EA4 rig dry-run on 2026-05-21 (EL9 cell).
Requires:      ea-apache24-mod_version
Requires:      libstdc++
Conflicts:     ea-apache24-mod_ruid2

%description
mod_pagespeed is an Apache module that rewrites HTML, CSS, JS, and image
assets at request time to reduce page load latency and bandwidth usage.
This package repackages the upstream mod_pagespeed 1.15 binary for use with
cPanel's EasyApache 4 Apache build.

ABI note: both targets share Apache 2.4 module magic number 20120211, so
the upstream binary (built against generic RHEL/AlmaLinux Apache 2.4) loads
into EA4's ea-apache24 without recompilation.

Documentation: https://modpagespeed.com/

%prep
# Set up an empty build dir; we don't have a source tarball — sources come
# from the upstream binary RPM extracted in %%build.
%setup -q -T -c

%build
# Repackager flow — no compilation. Extract the upstream binary RPM contents
# into the build dir.
rpm2cpio %{SOURCE1} | cpio -idmv

# Strip debug symbols from the module .so. Mirrors the original
# apache/incubator-pagespeed-cpanel spec.
%{__strip} -g usr/lib64/httpd/modules/mod_pagespeed.so

%install
rm -rf %{buildroot}

# 1. Module .so → EA4 modules dir.
install -d -m 0755 %{buildroot}%{_httpd_moddir}
install -m 0755 usr/lib64/httpd/modules/mod_pagespeed.so \
                %{buildroot}%{_httpd_moddir}/mod_pagespeed.so

# 2. LoadModule fragment → EA4 modconfdir. The 490_ prefix orders our module
# after mod_version (450_) and before most app-level modules; matches the
# numbering convention in the upstream apache/incubator-pagespeed-cpanel spec.
install -d -m 0755 %{buildroot}%{_httpd_modconfdir}
install -m 0644 %{SOURCE0} \
                %{buildroot}%{_httpd_modconfdir}/490_mod_pagespeed.conf

# 3. Full module config + libraries config → EA4 confdir. Upstream paths
# (/etc/httpd, /usr/lib64/httpd) are sed-repathed to EA4's layout
# (/etc/apache2, /usr/lib64/apache2).
#
# The upstream pagespeed.conf carries its OWN `LoadModule pagespeed_module`
# near the top (it is the all-in-one conf for stock Apache). On EA4 that is
# (a) a redundant SECOND loader — the canonical EA4 loader is our
# 490_mod_pagespeed.conf fragment in conf.modules.d, which loads first and
# emits the harmless "AH01574 already loaded, skipping" warning — and (b)
# UNGUARDED, so if the .so is ever absent this line hard-fails httpd startup
# (`Cannot load .../mod_pagespeed.so`), defeating the <IfFile> fail-safe we
# put on the 490_ fragment. Strip it here so the 490_ fragment is
# the single, <IfFile>-guarded loader; the rest of pagespeed.conf is wrapped
# in <IfModule pagespeed_module>, which goes inert when the module is absent.
install -d -m 0755 %{buildroot}%{_httpd_confdir}
sed -e 's|/etc/httpd/|/etc/apache2/|g' \
    -e 's|/usr/lib64/httpd/|/usr/lib64/apache2/|g' \
    -e '/^[[:space:]]*LoadModule[[:space:]]\{1,\}pagespeed_module/d' \
    etc/httpd/conf.d/pagespeed.conf \
    > %{buildroot}%{_httpd_confdir}/pagespeed.conf
sed -e 's|/etc/httpd/|/etc/apache2/|g' \
    -e 's|/usr/lib64/httpd/|/usr/lib64/apache2/|g' \
    etc/httpd/conf.d/pagespeed_libraries.conf \
    > %{buildroot}%{_httpd_confdir}/pagespeed_libraries.conf

# 4. Runtime cache + log dirs. ea-apache24 runs as nobody:nobody by default.
install -d -m 0750 %{buildroot}/var/cache/mod_pagespeed
install -d -m 0750 %{buildroot}/var/log/pagespeed

%files
%{_httpd_moddir}/mod_pagespeed.so
%{_httpd_modconfdir}/490_mod_pagespeed.conf
%config(noreplace) %{_httpd_confdir}/pagespeed.conf
%config            %{_httpd_confdir}/pagespeed_libraries.conf
%attr(0750,nobody,nobody) %dir /var/cache/mod_pagespeed
%attr(0750,nobody,nobody) %dir /var/log/pagespeed

%post
# Trigger an EA4 Apache reload so the new module takes effect. Skipped if the
# operator sets EA4_AUTO_RELOAD=0 prior to `yum install` (e.g., when batching
# multiple module installs).
if [ "${EA4_AUTO_RELOAD:-1}" = "1" ] && [ -x /scripts/restartsrv_httpd ]; then
    /scripts/restartsrv_httpd >/dev/null 2>&1 || true
fi

# Join the web-server user(s) to the `pagespeed` group, as on the other rpm
# channels. EA4 is a dep-free degraded channel: no optimizer package, hence no
# `pagespeed` group, so this is a no-op here by construction -- it only acts
# if an optimizer package ever lands on the host.
if getent group pagespeed >/dev/null 2>&1; then
    for webuser in apache nginx; do
        if id "$webuser" >/dev/null 2>&1; then
            usermod -a -G pagespeed "$webuser" || true
        fi
    done
fi

%postun
# On uninstall ($1 == 0), drop the LoadModule by reloading Apache.
if [ "$1" = "0" ] && [ -x /scripts/restartsrv_httpd ]; then
    /scripts/restartsrv_httpd >/dev/null 2>&1 || true
fi

%changelog
* Mon Jun 01 2026 Otto van der Schaaf <oschaaf@we-amp.com> - 2:1.15.0-2.cpanel
- Set Epoch: 2. cPanel's EA4 repo ships ea-apache24-mod_pagespeed at Epoch=1
  (1:1.13.35.2), which outranks our epoch-less 1.15.0 in dnf — a plain
  `dnf install ea-apache24-mod_pagespeed` / WHM EA4 "Customize" checkbox would
  install cPanel's stale 1.13.35.2 instead of ours. Epoch:2 makes ours win the
  default resolution. Verified on a real cPanel rig 2026-06-01.

* Mon Jun 01 2026 Otto van der Schaaf <oschaaf@we-amp.com> - 1.15.0-0.cpanel
- Renumber 1.1 -> 1.15: the maintained successor to Google's final
  mod_pagespeed 1.14.36.1. Repackages mod-pagespeed-1.15.0; the version field
  now reads 1.15.0. No functional change from the 1.1.0 line; package name
  ea-apache24-mod_pagespeed is unchanged.

* Thu May 21 2026 Otto van der Schaaf <oschaaf@we-amp.com> - 1.1.0-7.cpanel
- Add Requires: ea-apache24-mod_version. Default cPanel EA4 installs do not
  ship mod_version as part of the base ea-apache24 package; our LoadModule
  fragment in 490_mod_pagespeed.conf needs mod_version present at httpd
  parse time. Without this, httpd refuses to start on a stock EA4 host.
  Surfaced by the EA4 / cPanel end-to-end smoke rig on 2026-05-21 (EL9 cell).

* Tue May 19 2026 Otto van der Schaaf <oschaaf@we-amp.com> - 1.1.0-0.cpanel
- Initial We-Amp release as the maintained upstream of mod_pagespeed for
  EasyApache 4.
- Forked from apache/incubator-pagespeed-cpanel (archived 2023-04-21);
  repackages mod-pagespeed-1.1.0 (built against generic Apache 2.4) for
  EA4's ea-apache24 build.
