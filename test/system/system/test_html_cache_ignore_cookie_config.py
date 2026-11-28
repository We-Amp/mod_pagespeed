#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""DaemonHtmlCacheIgnoreCookie additions are checked when the configuration
is read.

With an optimizer library that publishes C API 1.15, an addition the
library's guard refuses (`*` alone, fewer than 3 characters before a `*`, or
a name that matches or would cover a session cookie) fails the
configuration test with an error that names it, and a narrow addition
passes. The check runs against a copy of the lane's configuration, so the
lane's own server is untouched. IIS is covered by its configuration test
(test/pagespeed/iis/iis_configuration_test.cc).
"""

import ctypes
import os
import re
import subprocess

import pytest

REFUSED = ["PHPSESSID", "wordpress_*", "ab*", "*"]
ACCEPTED = ["utm_src", "_my_ab_test"]


def _library_minor():
    """The C API minor of the optimizer library the server would load, or
    None when none loads."""
    try:
        lib = ctypes.CDLL("libpagespeed.so")
        lib.ps_version_minor.restype = ctypes.c_int
        return lib.ps_version_minor()
    except (OSError, AttributeError):
        return None


def _require_policy_library():
    minor = _library_minor()
    if minor is None:
        if os.environ.get("PAGESPEED_DAEMON_LANE") == "require":
            pytest.fail("the daemon lane is required but libpagespeed.so does not load")
        pytest.skip("no optimizer library: the ignore-cookie settings have no effect")
    if minor < 15:
        pytest.skip("the optimizer library predates C API 1.15: the settings have no effect")


def _apache_main_config(ctl: str):
    try:
        out = subprocess.run(
            ["sudo", "-n", ctl, "-V"], capture_output=True, text=True, timeout=60
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    root = re.search(r'-D HTTPD_ROOT="([^"]*)"', out)
    conf = re.search(r'-D SERVER_CONFIG_FILE="([^"]*)"', out)
    if not conf:
        return None
    path = conf.group(1)
    if not os.path.isabs(path):
        if not root:
            return None
        path = os.path.join(root.group(1), path)
    return path if os.path.exists(path) else None


def _check(server_config, addition: str):
    """Run the configuration test with one DaemonHtmlCacheIgnoreCookie line in
    the server-wide configuration; returns the CompletedProcess."""
    tmp = None
    try:
        if server_config.is_nginx:
            binary = os.environ.get("PAGESPEED_NGINX_BINARY")
            conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE")
            if not binary or not conf or not os.path.exists(conf):
                pytest.skip("the nginx binary / configuration of the lane are not known")
            conf_dir = os.path.dirname(conf)
            text = open(conf, encoding="utf-8").read()
            head, sep, rest = text.partition("    server {")
            assert sep, "no server block in the lane configuration"
            text = head + f"    pagespeed DaemonHtmlCacheIgnoreCookie {addition};\n" + sep + rest
            tmp = os.path.join(conf_dir, f"ignore-cookie-check-{os.getpid()}.conf")
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            command = [binary, "-t", "-p", conf_dir, "-c", tmp]
        elif server_config.server_type == "apache":
            ctl = os.environ.get("PAGESPEED_APACHE_CTL", "apache2ctl")
            main_conf = _apache_main_config(ctl)
            if main_conf is None:
                pytest.skip("the main Apache configuration file of this rig is not known")
            tmp = f"/tmp/ignore-cookie-check-{os.getpid()}.conf"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(f"Include {main_conf}\n")
                f.write(f"ModPagespeedDaemonHtmlCacheIgnoreCookie {addition}\n")
            command = ["sudo", "-n", ctl, "-t", "-f", tmp]
        else:
            pytest.skip("configuration syntax check is driven for Apache and nginx only")
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    except FileNotFoundError:
        pytest.skip("the server binary needed for the syntax check is not available")
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)
    if "a password is required" in result.stdout + result.stderr:
        pytest.skip("passwordless sudo is required to run the syntax check")
    return result


@pytest.mark.parametrize("addition", REFUSED)
def test_a_refused_addition_fails_the_configuration_test(server_config, addition):
    _require_policy_library()
    result = _check(server_config, addition)
    output = result.stdout + result.stderr
    assert result.returncode != 0, output
    assert f"DaemonHtmlCacheIgnoreCookie {addition} is refused" in output, output


@pytest.mark.parametrize("addition", ACCEPTED)
def test_a_narrow_addition_passes_the_configuration_test(server_config, addition):
    _require_policy_library()
    result = _check(server_config, addition)
    assert result.returncode == 0, result.stdout + result.stderr


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
