- **`FetchWithGzip on` now works for gzip-compressed resources.** On ports
  that fetch with the libcurl-based fetcher (Apache, IIS, Envoy, and nginx
  when it uses that fetcher), a resource the origin served gzip-compressed
  was decoded twice, so the fetch failed and the resource was not rewritten.
  It is now decoded once and rewritten as usual.
