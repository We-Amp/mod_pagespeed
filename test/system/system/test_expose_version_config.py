#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""ExposeVersionInHeaders is a server-wide option.

Set in the main (server-wide) configuration it passes the configuration
test; set inside a <VirtualHost> (Apache) or a server block (nginx) it makes
the configuration test fail with an error that names the directive.

The check runs against a copy of the lane's configuration, so the lane's own
server is untouched. The header values themselves are asserted by the header
tests, in the default mode and with PAGESPEED_TEST_EXPOSE_VERSION=1. IIS and
Envoy are covered by their configuration unit tests.
"""

import os
import re
import subprocess

import pytest


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


def _check(server_config, scope: str):
    """Run the configuration test with the option at `scope` ("global" or
    "vhost"); returns the CompletedProcess."""
    tmp = None
    try:
        if server_config.is_nginx:
            binary = os.environ.get("PAGESPEED_NGINX_BINARY")
            conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE")
            if not binary or not conf or not os.path.exists(conf):
                pytest.skip("the nginx binary / configuration of the lane are not known")
            conf_dir = os.path.dirname(conf)
            text = open(conf, encoding="utf-8").read()
            line = "pagespeed ExposeVersionInHeaders on;\n"
            if scope == "global":
                head, sep, rest = text.partition("    server {")
                assert sep, "no server block in the lane configuration"
                text = head + "    " + line + sep + rest
            else:
                head, sep, rest = text.partition("    server {\n")
                assert sep, "no server block in the lane configuration"
                text = head + sep + "        " + line + rest
            tmp = os.path.join(conf_dir, f"expose-version-check-{os.getpid()}.conf")
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            command = [binary, "-t", "-p", conf_dir, "-c", tmp]
        elif server_config.server_type == "apache":
            ctl = os.environ.get("PAGESPEED_APACHE_CTL", "apache2ctl")
            main_conf = _apache_main_config(ctl)
            if main_conf is None:
                pytest.skip("the main Apache configuration file of this rig is not known")
            tmp = f"/tmp/expose-version-check-{os.getpid()}.conf"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(f"Include {main_conf}\n")
                if scope == "global":
                    f.write("ModPagespeedExposeVersionInHeaders on\n")
                else:
                    f.write("<VirtualHost *:65097>\n  ServerName expose.example.com\n")
                    f.write("  ModPagespeedExposeVersionInHeaders on\n</VirtualHost>\n")
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


def test_server_wide_use_passes_the_configuration_test(server_config):
    result = _check(server_config, "global")
    assert result.returncode == 0, result.stdout + result.stderr


def test_vhost_use_fails_the_configuration_test(server_config):
    result = _check(server_config, "vhost")
    output = result.stdout + result.stderr
    assert result.returncode != 0, output
    assert "ExposeVersionInHeaders" in output, output


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
