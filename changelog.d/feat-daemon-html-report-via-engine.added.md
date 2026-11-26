- **`DaemonHtmlCache serve-variants` is accepted, and for now runs as
  report-only.** `serve-variants` is the mode that will store HTML pages and
  serve their Markdown copies to clients that ask for one. In this release no
  server serves anything in it: on Apache and nginx it behaves exactly like
  `report-only` (the response is never changed, nothing is stored, the
  `daemon_html_*` counters count and the decision log writes `report-only`
  lines), and on IIS and Envoy it is counted as `daemon_html_unavailable`, as
  `report-only` is there. The value `serve-cached-html` is reserved for a
  later release: setting it logs a warning and leaves the option as it was,
  so a configuration written for that release still loads.
- **The HTML cache decision log names what the server configuration added.**
  With an optimizer library that publishes C API 1.14, a report-only line on
  Apache carries `vhost_added`: the names (never the values) of the response
  headers that the server's own configuration, such as `mod_headers` or
  `mod_expires`, added or changed after the module recorded the response.
  nginx reads a single header view, so its lines carry no `vhost_added`.
