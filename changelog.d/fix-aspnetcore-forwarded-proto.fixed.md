- **ASP.NET Core (Inverse mode): optimized requests keep the client's scheme
  behind a TLS terminator.** When the middleware forwarded an optimizable
  request to the bundled nginx, it replaced any inbound
  `X-Forwarded-Proto` with the loopback connection's `http`. An app deployed
  behind a TLS terminator or load balancer (the documented HTTPS setup, with
  `UsePageSpeed()` before `UseForwardedHeaders()`) therefore saw `http` on
  every optimized request: with `UseHttpsRedirection()` every optimized page
  redirected to itself in a loop, and absolute URLs the app built (canonical
  links, OAuth redirect URIs, `Location` headers) came out as `http`. The
  middleware now passes an inbound `X-Forwarded-Proto` through unchanged on a
  plain-HTTP connection and uses the request's own scheme when the header is
  absent; on an HTTPS connection the request's real scheme always wins, so a
  client-supplied header cannot downgrade it. Local HTTP testing was never
  affected.
- **ASP.NET Core: `ExcludePaths` now means one thing.** The option was
  documented as regular expressions, but the middleware matched each entry as
  a path prefix or a glob while the generated nginx configuration pasted it
  verbatim into a regex `location`. Which syntax an operator followed decided
  the failure: `*.json` produced an invalid nginx configuration so the
  application refused to start; `/admin/*` also turned optimization off for
  unrelated paths such as `/docs/administration`; and the documented
  `^/api/` never matched in the middleware, so those requests paid the extra
  loopback hop for nothing. Every entry is now a path prefix (`/api/`) or a
  glob (`/admin/*`, `*.json`; `*` matches any sequence, `?` any single
  character), and the middleware and the generated nginx configuration
  apply every entry the same way, excluding exactly the same paths.
  **Action required on upgrade:** regular-expression entries are no longer
  supported. An entry that starts with `^`, ends with `$`, or uses
  characters outside the allowed set is rejected at startup with a clear
  message naming the accepted syntax; the documented example `^/api/`
  becomes `/api/`. A regular expression built only from allowed characters
  is not rejected: it is read as a glob, in which `.` is a literal dot, so
  the regex form `/api/.*` matches only paths starting with `/api/.` and
  must be rewritten as the prefix `/api/`; a startup warning flags entries
  containing `.*` or `.+`.
