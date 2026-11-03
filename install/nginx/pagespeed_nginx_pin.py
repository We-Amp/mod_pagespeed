# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""dnf plugin of nginx-module-pagespeed-nginxorg: hold nginx at a version the
module has a build for (dnf 4, Enterprise Linux 9).

The module package requires nginx at exactly the version it was built for. When
nginx.org publishes a newer nginx before the matching module build exists, a
plain `dnf update` fails as a whole: its best candidate for nginx cannot be
installed. While the module is installed, this plugin hides, from a command
that resolves a transaction, the nginx builds that no installed or available
nginx-module-pagespeed-nginxorg package can run with, and the available
packages that require exactly such an nginx (nginx.org's own module packages),
so `dnf update` takes every other update and keeps nginx, and moves nginx and
the module together once the matching module build is available. The pin
itself stays exact.

It does not hide anything from commands that only report (`dnf check-update`,
`dnf list`, `dnf repoquery`), so a pending nginx update stays visible there, or
from a command whose package arguments name nginx itself (`nginx`,
`nginx.x86_64`, `nginx-<version>`): those are left to dnf's own rules, so moving
the pair by hand works (`dnf install nginx-2:1.30.6 <module package>`), and an
nginx update the module cannot follow fails with dnf's explanation rather than
doing nothing. To take an nginx update ahead of the module (for example a
security fix), remove the module first, or upgrade nginx by name with
`--allowerasing`, which removes the module.

If the module's requirement is not of the form `nginx = E:V`, the plugin hides
nothing (dnf behaves as without it).
"""

import logging
import re

import dnf

logger = logging.getLogger("dnf.plugin")

MODULE = "nginx-module-pagespeed-nginxorg"

# A package argument that names nginx itself: nginx, nginx.x86_64,
# nginx-1.30.6, nginx-2:1.30.6-1.el9.ngx (not nginx-module-*).
NAMES_NGINX = re.compile(r"^nginx(\.[A-Za-z0-9_]+|-[0-9].*)?$")


def _specs(cli):
    """The package arguments of the command, wherever dnf 4 keeps them."""
    opts = getattr(getattr(cli, "command", None), "opts", None)
    out = []
    for attr in ("pkg_specs", "grp_specs", "package", "packages"):
        value = getattr(opts, attr, None)
        if isinstance(value, str):
            out.append(value)
        elif value:
            out.extend(str(v) for v in value)
    return out


def _requirements(pkg):
    """(exact, versions): the (epoch, version) pairs pkg requires nginx at with
    `nginx = E:V`, and the versions of the `nginx-r<version>` it requires (the
    provide nginx.org's nginx carries, which its module packages require)."""
    exact, versions = set(), set()
    for req in pkg.requires:
        parts = str(req).split()
        if len(parts) == 3 and parts[0] == "nginx" and parts[1] == "=":
            epoch, _, rest = parts[2].rpartition(":")
            version = rest.split("-", 1)[0]
            exact.add((int(epoch or 0), version))
            versions.add(version)
        elif len(parts) == 1 and parts[0].startswith("nginx-r"):
            versions.add(parts[0][len("nginx-r"):])
    return exact, versions


class PagespeedNginxPin(dnf.Plugin):
    name = "pagespeed_nginx_pin"

    def sack(self):
        cli = self.cli
        if cli is not None:
            if not getattr(getattr(cli, "demands", None), "resolving", True):
                return
            if any(NAMES_NGINX.match(spec) for spec in _specs(cli)):
                return
        q = self.base.sack.query()
        if not q.installed().filter(name=MODULE):
            return
        allowed = set()
        for pkg in q.filter(name=MODULE):
            allowed |= _requirements(pkg)[0]
        if not allowed:
            return
        allowed_versions = {v for _e, v in allowed}
        avail = q.available()
        hidden = [p for p in avail.filter(name="nginx") if (p.epoch, p.version) not in allowed]
        if not hidden:
            return
        # Packages that run with one nginx only, other than nginx itself.
        dependent = avail.filter(requires__glob=["nginx", "nginx-r*"]).filter(name__neq="nginx")
        for p in dependent:
            exact, versions = _requirements(p)
            if exact and not exact & allowed:
                hidden.append(p)
            elif versions and not versions & allowed_versions:
                hidden.append(p)
        self.base.sack.add_excludes(q.filter(pkg=hidden))
        logger.info("%s: nginx is held at the version(s) %s has a build for (%s); "
                    "%d package(s) for other nginx versions are hidden from this command",
                    self.name, MODULE, ", ".join(sorted(allowed_versions)), len(hidden))
