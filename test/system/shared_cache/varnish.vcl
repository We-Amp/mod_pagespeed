# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
vcl 4.1;
# A shared cache with Varnish's built-in behaviour: one copy per URL and Host,
# and per value of each request header the response's Vary names.
backend default { .host = "origin"; .port = "80"; }
sub vcl_recv { set req.http.Host = "localhost"; }
sub vcl_deliver {
  if (obj.hits > 0) { set resp.http.X-Proxy-Cache = "HIT"; } else { set resp.http.X-Proxy-Cache = "MISS"; }
}
