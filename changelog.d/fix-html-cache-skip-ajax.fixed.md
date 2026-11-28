- **The HTML cache never stores or answers a script's request.** A request
  whose `X-Requested-With` contains `XMLHttpRequest` anywhere in its value
  (compared without regard to case, so also when repeated lines were joined
  into one value), or that carries
  a non-empty `X-MicrosoftAjax` or `X-Prototype-Version` header, is no longer
  stored by `DaemonHtmlCache serve-variants` on Apache and nginx. A page
  fragment an origin returns to such a request can therefore no longer be
  kept under the page's address or become the source of its Markdown copy.
  On Apache, nginx and IIS, such a request is also never answered with the
  Markdown copy; it goes to the origin. Another `X-Requested-With` value,
  such as the application name an Android WebView sends, is not affected.
  Each refusal is counted in `daemon_html_refused_request_header` at the
  store and, for a Markdown request, in
  `daemon_markdown_fallthrough_request_header` (with optimizer C API 1.15 also
  in `daemon_html_refused_request_header`) at the lookup. In
  `report-only` these requests are no longer evaluated: they are left out of
  `daemon_html_evaluated` and the report's refusal reasons, and count only in
  `daemon_html_refused_request_header`.
