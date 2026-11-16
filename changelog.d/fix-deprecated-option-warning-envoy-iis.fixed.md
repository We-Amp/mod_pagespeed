- **Envoy and IIS now say when a configured option name is deprecated.** A
  configuration that still names a removed option, such as
  `pagespeed FileCacheInodeLimit`, was already accepted on these ports but
  produced a generic "unknown option" warning. It now logs the same message as
  nginx: the option is deprecated, has no effect, and should be
  removed from the configuration. Names that were never options still warn as
  before.
