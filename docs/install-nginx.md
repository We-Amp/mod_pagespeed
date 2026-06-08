# Installing PageSpeed for Nginx

## Requirements

- A supported Linux distribution running its **stock** nginx package
  (Debian 11/12/13, Ubuntu 22.04/24.04, AlmaLinux/RHEL/Rocky 9).
- `x86_64` or `arm64`.

The prebuilt module is a dynamic nginx module, and nginx pins dynamic modules to
the **exact** nginx version they were built against (down to the patch;
`--with-compat` does not relax this). The published packages are therefore built
against each distribution's **stock** nginx. If you run a *custom* nginx —
including the **nginx.org** repository's `nginx` (e.g. the 1.30.x stable line) —
no published package matches it, and the module **cannot be built from source
externally** because the PageSpeed module source is not public. Use your
distribution's stock nginx.

## Install

Add the We-Amp package repository (once), then install. All release artifacts are
also listed on the [downloads page](https://modpagespeed.com/1.1/docs/downloads/).

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh

sudo apt install nginx-module-pagespeed     # Debian / Ubuntu
sudo dnf install nginx-module-pagespeed     # RHEL / AlmaLinux / Rocky 9

sudo nginx -t && sudo systemctl reload nginx
```

On stock nginx the package's `load_module` directive is auto-enabled, so no
manual `nginx.conf` edit is needed.

## Configuration

```nginx
load_module modules/ngx_pagespeed_module.so;

http {
    # ...

    server {
        listen 80;
        server_name example.com;

        # Enable PageSpeed. This activates the CoreFilters rewrite level by
        # default (29 optimization filters, including combine_css,
        # combine_javascript, and rewrite_images).
        pagespeed on;
        pagespeed FileCachePath /var/cache/ngx_pagespeed;

        # Admin console (restrict access in production)
        pagespeed AdminPath /pagespeed_admin;
        pagespeed StatisticsPath /pagespeed_statistics;

        # Enable additional filters outside the default CoreFilters set.
        # collapse_whitespace, insert_image_dimensions, and defer_javascript
        # are NOT in CoreFilters and must be enabled explicitly.
        # See filter-reference.md for filter membership.
        pagespeed EnableFilters collapse_whitespace,insert_image_dimensions;

        # Ensure PageSpeed resource requests are handled
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon$" { }

        location / {
            proxy_pass http://backend;
        }
    }
}
```

## Cache Directory Setup

```bash
sudo mkdir -p /var/cache/ngx_pagespeed
sudo chown www-data:www-data /var/cache/ngx_pagespeed
```

## Verification

```bash
sudo nginx -t
sudo systemctl restart nginx
curl -I http://localhost/
# Should show an X-PageSpeed: <version> response header
```

## System Tests

```bash
./test/system/run_nginx_tests.sh
```

Expected: 169 pass, 40 skip, 0 fail.

## Known Limitations

- Lazyload images filter may time out
- Inline preview images filter may time out
- Canonicalize JS libraries filter may time out
- ETag not added to `.pagespeed.` resources
- Chunked encoding differences affect Content-Length tests

See [test-catalog.md](test-catalog.md) for full details.

## Next steps: configuration & operations

By default (`pagespeed on;`) the `CoreFilters` rewrite level is active, which enables 29 optimization filters automatically; a bare `pagespeed on;` is not a no-op. To enable additional filters not in CoreFilters (for example `collapse_whitespace` or `defer_javascript`), use `pagespeed EnableFilters`. To enable all non-dangerous filters, use `pagespeed RewriteLevel AllFilters`. For a smaller footprint, use `pagespeed RewriteLevel OptimizeForBandwidth`, or `pagespeed RewriteLevel PassThrough` to disable all default optimization.

Full configuration and operations documentation lives on modpagespeed.com:

- [Filter selection](https://modpagespeed.com/1.1/docs/filter-selection/) — rewrite levels and per-filter enable/disable
- [Filter reference](https://modpagespeed.com/1.1/docs/filter-reference/) — what each filter does and which level enables it
- [Admin console](https://modpagespeed.com/1.1/docs/admin-console/) — statistics, cache inspection, and cache purging
