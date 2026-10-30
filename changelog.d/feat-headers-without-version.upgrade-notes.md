- **If a script or monitor reads the version from the `X-Mod-Pagespeed` or
  `X-Page-Speed` header, set `ExposeVersionInHeaders on` in the server-wide
  configuration as part of the upgrade.** The option is accepted only
  server-wide (a use inside a virtual host or server block fails the
  configuration test), and releases older than this one reject it as unknown,
  so add it with the upgrade, not before.
