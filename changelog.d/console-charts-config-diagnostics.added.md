- **The admin console Configuration page can be searched and compared.** A
  filter box narrows the text to matching lines and says how many match, Copy
  copies the whole text, and lines that differ between the server's
  configuration and the one in effect for this request are marked "differs".
  When the text has its usual shape, filters show as a list and options as a
  table; any other text is still shown and searched line by line.
- **The Support page always shows a diagnostics bundle you can copy or
  download.** It holds what support needs to act: the console's scope, start
  times, findings, the optimizer's checks, a dozen key counters, the cache and
  purge summary, the browser and the detected server, and it ends with a list
  of what was left out. Query strings and public client addresses are removed,
  cookies, credentials and tokens are always redacted, and the configuration
  text is included only when you turn it on, with secret-looking values
  redacted.
- **The Overview says what changed since your last visit**, for example that
  the web server restarted or reloaded, the configuration changed, or the
  module or optimizer version changed. The record is kept only in your
  browser; a first visit, or a visit where nothing changed, shows nothing.
- **Savings explains why some content was not optimized.** A "Not optimized,
  by reason" list names each reason the module counted, such as hosts it is
  not authorized to rewrite, resources whose caching headers make them unsafe
  to optimize, CSS or JavaScript the parser or minifier could not process, and
  image work dropped under load, with a fix for each. Image work dropped under
  load is also an Overview finding while it keeps happening.
