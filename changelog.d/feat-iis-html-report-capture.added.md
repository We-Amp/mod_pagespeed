- **`DaemonHtmlCache report-only` works on IIS.** The IIS module now evaluates
  the HTML pages it rewrites, as Apache and nginx do, instead of counting them
  as `daemon_html_unavailable`: the `daemon_html_*` counters count and the
  decision log writes a line for each page. Nothing a client receives changes.
  The evaluation reads the response twice: as the page's handler produced it,
  and as IIS sends it, after the site's `customHeaders` and dynamic
  compression, so an operator `Cache-Control: no-store` set in `customHeaders`
  is seen. A page that static compression served compressed is evaluated on
  its decoded bytes.
