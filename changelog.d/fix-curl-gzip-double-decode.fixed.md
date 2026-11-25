- **`FetchWithGzip on` now works for compressed resources.** On ports that
  fetch with the libcurl-based fetcher (Apache, IIS, Envoy, and nginx when it
  uses that fetcher), a resource the origin served gzip- or deflate-compressed
  was decoded twice. Where the module itself needed the content, the fetch
  failed and the resource was not rewritten. Where the request accepted gzip,
  the fetch succeeded but handed on the decoded body still labelled as
  compressed, and that response could be stored in the cache, so clients that
  accept gzip could receive a body they cannot decode. This happened with
  per-virtual-host statistics on or off. The body is now decoded exactly once,
  and a request that accepts gzip gets the compressed bytes as the origin sent
  them.
