- **Hardening: the diagnostics bundle redacts more.** With the configuration
  text added, experiment settings keep an option's value only when the option
  is known to be safe to share, and the domain mappings keep their shape with
  every domain and host replaced. Session ids, signatures and a password in an
  address without a user name are redacted too, and the configuration text
  now starts each experiment on a line of its own.
