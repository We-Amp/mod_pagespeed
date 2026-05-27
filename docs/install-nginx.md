# Installing PageSpeed for Nginx

## Requirements

- Nginx 1.30.x (stable) or 1.29.x (mainline)
- Linux x86_64

## Install Pre-built Module

```bash
tar xzf ngx_pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
sudo cp ngx_pagespeed-1.1.0-beta.1/ngx_pagespeed_module.so /usr/lib/nginx/modules/
```

Add to `nginx.conf` (before the `http` block):
```nginx
load_module modules/ngx_pagespeed_module.so;
```

## Build from Source

### Using the build script

```bash
./scripts/build_nginx_with_pagespeed.sh --nginx-version=1.30.1
```

### Using Bazel (inside Docker)

```bash
docker compose up -d
docker compose exec dev bash

bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so

# Module is at: bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so
```

## Configuration

```nginx
load_module modules/ngx_pagespeed_module.so;

http {
    # ...

    server {
        listen 80;
        server_name example.com;

        # Enable PageSpeed
        pagespeed on;
        pagespeed FileCachePath /var/cache/ngx_pagespeed;

        # Admin console (restrict access in production)
        pagespeed AdminPath /pagespeed_admin;
        pagespeed StatisticsPath /pagespeed_statistics;

        # Enable filters
        pagespeed EnableFilters combine_css,combine_javascript;
        pagespeed EnableFilters rewrite_images;
        pagespeed EnableFilters collapse_whitespace;

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
# Should show: X-PageSpeed: 1.1.0-beta.1
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
