- **`serve-cached-html` can answer HTML requests from the stored page (engine).**
  With a C API 1.15 library, the HTML cache engine can now answer a request
  from the stored origin page, for the server to replay through its rewriter.
  It only does so when every one of these holds: the library says the entry is
  fresh, under the `DaemonHtmlCacheMaxAge` cap and never past the origin's own
  lifetime; it is never revalidated, and a conditional request always goes to
  the origin. The entry was recorded anonymously under the current credential
  policy, and that is checked again before the answer is sent. The answer never
  carries `Set-Cookie`, `ETag` or `Last-Modified`. A served page is never
  recorded or stored again. Under the default scope `agents`, a request from an
  unknown agent with no Markdown preference reads nothing from the cache. A
  Markdown request whose copy cannot be served may get the page instead. A
  `POST`, `PUT`, `DELETE` or other unsafe request answered with a success or
  redirect status at once fences the page in this process, plus up to two
  same-host targets that its `Location` and `Content-Location` lines name
  (relative references resolved against the request), and removes them from
  the cache. New counters report pages served by agent class, the agent HTML
  requests evaluated, a sampled estimate of what scope `all` would serve,
  unsafe-request invalidations, and checks that must stay at zero.
  The Apache, nginx and IIS ports start sending these answers, and invalidating
  on unsafe requests, in a later release; until then no response changes, and
  a request without a Markdown preference is counted as before.
