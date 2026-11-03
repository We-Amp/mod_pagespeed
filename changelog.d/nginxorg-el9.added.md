- **nginx.org's nginx on Enterprise Linux 9.** The nginx.org module package
  `nginx-module-pagespeed-nginxorg` is now also available for AlmaLinux, Rocky
  Linux and RHEL 9 (x86_64), for nginx.org's stable nginx. Like the Debian and
  Ubuntu builds, each build is for one nginx version. Because dnf otherwise
  refuses a whole `dnf update` while nginx.org is one nginx release ahead of the
  matching build, the package installs a small dnf plugin that holds nginx (and
  nginx.org's module packages for it) until that build is available; every other
  update goes through, and `dnf check-update` still lists the pending nginx. To
  take an nginx update first, for example a security fix, run
  `dnf upgrade nginx --allowerasing`, which removes the module package. The
  plugin is removed with the package. Tested on AlmaLinux 9; updates through
  PackageKit or Cockpit, and Rocky Linux and RHEL, are not tested yet. Details:
  docs/install-nginx.md.
