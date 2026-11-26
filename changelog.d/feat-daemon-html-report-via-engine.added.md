- **`DaemonHtmlCache serve-variants` stores HTML pages and serves their
  Markdown copies on Apache and nginx.** In this mode the server keeps a copy
  of each HTML page that its headers allow a shared cache to keep, and answers
  a request that prefers `text/markdown` with the Markdown copy that the
  optimizer worker derives from it; every other request gets the HTML page,
  unchanged. It needs `AgentOptimize on`, an optimizer library that publishes
  C API 1.14 and a worker started with `--html-mode markdown-static` (or
  `markdown-rendered`) and `--agent-optimize`; restart the optimizer before
  reloading the web server. Start with `report-only` and read its counters and
  decision log first. Without those conditions the mode runs as `report-only`
  and logs why when the state changes. IIS does not serve Markdown yet: there,
  `serve-variants` runs as `report-only` (the response is never changed and
  nothing is stored), and on Envoy it is counted as `daemon_html_unavailable`,
  as `report-only` is. The Apache and nginx entries in this section list what
  each server stores and serves, and the guide in
  `docs/html-cache-serve-variants.md` covers the steps and the limits. The value `serve-cached-html` is reserved for a later release:
  setting it logs a warning and leaves the option as it was, so a configuration
  written for that release still loads.
- **The HTML cache decision log names what the server configuration added.**
  With an optimizer library that publishes C API 1.14, a report-only line on
  Apache carries `vhost_added`: the names (never the values) of the response
  headers that the server's own configuration, such as `mod_headers` or
  `mod_expires`, added or changed after the module recorded the response.
  nginx reads a single header view, so its lines carry no `vhost_added`.
