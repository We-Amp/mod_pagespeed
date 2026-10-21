# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
vcl 4.1;
# A shared cache that keys image responses on a device class instead of the
# raw User-Agent: for an image request, the User-Agent is replaced with one
# representative string per class before the lookup and before the request
# goes to the origin, so the response's Vary: User-Agent splits the cache into
# a handful of copies and the origin selects for the same class.  The rules
# are the optimizer's classifier at the pinned release (see "Behind a shared
# cache or CDN" in docs/daemon-adapter-deployment.md); the same block works
# in Fastly's vcl_recv.
backend default { .host = "origin"; .port = "80"; }

sub vcl_recv {
  set req.http.Host = "localhost";
  if (req.url ~ "(?i)\.(jpe?g|png|gif|webp|avif)(\?|$)") {
    if (req.http.User-Agent ~ "(?i)ipad|tablet" ||
        (req.http.User-Agent ~ "(?i)android" && req.http.User-Agent !~ "(?i)mobile")) {
      if (req.http.User-Agent ~ "2x|Retina") {
        set req.http.User-Agent = "device-class tablet Retina";
      } else {
        set req.http.User-Agent = "device-class tablet";
      }
    } elsif (req.http.User-Agent ~ "(?i)mobile|android|iphone|ipod") {
      set req.http.User-Agent = "device-class Mobile";
    } elsif (req.http.User-Agent ~ "2x|Retina") {
      set req.http.User-Agent = "device-class Retina";
    } else {
      set req.http.User-Agent = "device-class desktop";
    }
  }
}

sub vcl_deliver {
  if (obj.hits > 0) { set resp.http.X-Proxy-Cache = "HIT"; } else { set resp.http.X-Proxy-Cache = "MISS"; }
}
