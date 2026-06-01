# Installing mod_pagespeed for Apache

## Requirements

- Apache 2.4+
- Linux x86_64 (Ubuntu 22.04+, Debian 12+, RHEL 9+, Rocky 9+)

## Install from Package

### Ubuntu/Debian (.deb)

```bash
sudo dpkg -i mod-pagespeed-beta_1.1.0-beta.1_amd64.deb
sudo apt-get -f install  # resolve dependencies if needed
```

The package automatically:
- Installs `mod_pagespeed.so` to `/usr/lib/apache2/modules/`
- Creates config files in `/etc/apache2/mods-available/`
- Creates cache directory at `/var/cache/mod_pagespeed/`

Enable the module:
```bash
sudo a2enmod pagespeed
sudo systemctl restart apache2
```

### RHEL/Rocky (.rpm)

```bash
sudo rpm -i mod-pagespeed-beta-1.1.0-beta.1.x86_64.rpm
sudo systemctl restart httpd
```

The package installs:
- Module to `/usr/lib64/httpd/modules/mod_pagespeed.so`
- Config to `/etc/httpd/conf.d/pagespeed.conf`

## Build from Source

```bash
docker compose up -d
docker compose exec dev bash

bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so

# Module is at: bazel-bin/libmod_pagespeed.so
```

Install manually:
```bash
sudo cp bazel-bin/libmod_pagespeed.so /usr/lib/apache2/modules/mod_pagespeed.so
sudo mkdir -p /var/cache/mod_pagespeed /var/log/pagespeed
sudo chown www-data:www-data /var/cache/mod_pagespeed /var/log/pagespeed
```

## Configuration

Basic configuration in Apache config:
```apache
LoadModule pagespeed_module /usr/lib/apache2/modules/mod_pagespeed.so

<IfModule pagespeed_module>
    ModPagespeed on
    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"

    # SSL certificates for outbound HTTPS fetching
    ModPagespeedSslCertDirectory /etc/ssl/certs

    # Enable specific filters
    ModPagespeedEnableFilters combine_css,combine_javascript
    ModPagespeedEnableFilters rewrite_images
    ModPagespeedEnableFilters collapse_whitespace
</IfModule>
```

## Verification

```bash
curl -I http://localhost/
# Should show: X-PageSpeed: 1.1.0-beta.1
```

## System Tests

```bash
./test/system/run_system_tests.sh
./test/system/run_system_tests.sh -k sanity  # Quick sanity check
```

Expected: 195 pass, 14 skip, 0 fail.
