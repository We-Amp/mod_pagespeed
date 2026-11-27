- **The HTML cache counts requests by agent class.** On a server with
  `DaemonHtmlCache` on, each request is classified once, internal redirects
  included: `daemon_html_agent_class_c3` counts a `User-Agent` that names a
  known AI agent product token (from a list compiled into the module, matched
  as whole tokens, case-insensitively), `daemon_html_agent_class_c1` a request
  with a valid Web Bot Auth signature (nginx with `WebBotAuth on`), and
  `daemon_html_agent_class_c4` every other request.
  `daemon_html_agent_class_c1_not_ready` and
  `daemon_html_agent_class_signature_failed` count requests whose signature
  verdict was not available or did not verify; they get their `User-Agent`'s
  class. The class is what the client claims and changes nothing in this
  release; it is the input of the upcoming serve scope.
