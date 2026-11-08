# Envoy filter example

> **Experimental. Not for production.** The Envoy port is built from source
> only, has no packaged release, and is not covered by the release security
> statements. Its administrative paths answer every client unless `admin_auth`
> is enabled. Run it only on a trusted network, behind authentication, and
> expect no support or security commitment.

This project implements `PageSpeed` as an `Envoy`-filter.

## Building

To build the Envoy static binary:

1. `bazel build //pagespeed/envoy:envoy`

## Testing

To run the pagespeed envoy filter integration test:

`bazel test //pagespeed/envoy:http_filter_integration_test`


## Configuration


```yaml
http_filters:
- name: pagespeed          # before envoy.router because order matters!
  config:
    key: via
    val: pagespeed-filter
- name: envoy.router
  config: {}
```
