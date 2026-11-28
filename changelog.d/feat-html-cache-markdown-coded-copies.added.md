- **The HTML cache sends the Markdown copy compressed when the client asks
  for it** (Apache, nginx, IIS). With an optimizer library that publishes C API
  1.16, a request for a page's Markdown copy that accepts `br` or `gzip` gets
  the brotli or gzip copy the optimizer stored beside the plain one, chosen by
  the request's `Accept-Encoding` (q-values honoured, `identity;q=0` and
  `*` included). The copy is compressed once by the optimizer, never per
  request, and the server's own compression (`mod_deflate`, `mod_brotli`,
  nginx `gzip` and `brotli`, IIS dynamic compression) never compresses it
  again. A coded answer carries `Content-Encoding`, the coded length and an
  entity tag of its own, so a `304` is answered for the coding the client
  holds; every Markdown answer now lists `Accept-Encoding` in its `Vary`
  line. A page whose plain copy was stored before the optimizer wrote coded
  copies gets them after its first Markdown request: the module asks the
  optimizer once per page. A request that accepts no coding the copy exists
  in goes to the origin. New counters: `daemon_markdown_served_br`,
  `daemon_markdown_served_gzip`, `daemon_markdown_served_identity`,
  `daemon_markdown_encoded_copy_missing`,
  `daemon_markdown_encoded_copy_unbound`,
  `daemon_markdown_compress_demand_sent`, `daemon_markdown_coded_fallback`
  (nginx sent the plain copy because a body filter such as `sub_filter`
  would have rewritten the compressed one) and
  `daemon_markdown_fallthrough_no_acceptable_encoding`. With an older
  optimizer library the plain copy is sent exactly as before.
