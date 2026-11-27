- **The HTML cache never stores or answers a script's request.** A request
  that carries `X-Requested-With: XMLHttpRequest` (the value compared without
  regard to case), or any non-empty `X-MicrosoftAjax` or `X-Prototype-Version`
  header, is no longer stored by `DaemonHtmlCache serve-variants` on Apache
  and nginx, so a page fragment an origin returns to such a request can no
  longer be kept under the page's address or become the source of its
  Markdown copy. On every port, such a request is also never answered with
  the Markdown copy; it goes to the origin. Another `X-Requested-With` value,
  such as the application name an Android WebView sends, is not affected.
  Each refusal is counted in `daemon_html_refused_request_header` (store) or
  `daemon_markdown_fallthrough_request_header` (Markdown request).
