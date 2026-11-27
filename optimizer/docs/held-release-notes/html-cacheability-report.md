Held until the HTML cacheability report is announced; do not add this to CHANGELOG.md before then.

Added: the C API (PS_API 1.12) gains an HTML cacheability report:
`ps_html_store_classify` reports whether a `text/html` response would be
stored, `ps_html_decision_format` writes one decision as a single JSON line
whose keys and values are a closed set of codes and counts, and
`ps_html_store_reason_name` names the reasons. Nothing in this API writes or
serves cached HTML. A page would be stored only when the origin stated its
lifetime or sent a validator, never on a guessed lifetime; pages that look
personalised are refused (a request with credentials, or `Cache-Control:
no-store`, or a cookie unless the response is explicitly `public` or carries a
valid `s-maxage`; a response that sets a cookie, `private` or `no-store`, or a
`Vary` beyond `Accept-Encoding`), a CSRF token or a login or checkout form in
the body is refused with its own reason, and URLs that cannot be keyed safely
are refused, including the optimizer's own rewritten URLs. Conflicting
`max-age` or `s-maxage` directives and repeated `Expires` lines count as a
lifetime of zero, and all three HTTP date forms are read. No behaviour changes
in this release: nothing in the nginx module, the worker or the ASP.NET
middleware calls the new entry points yet.
