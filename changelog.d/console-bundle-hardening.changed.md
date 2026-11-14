- **Hardening: the diagnostics bundle redacts more.** With the configuration
  text added, it keeps only the values of options known to be safe to share,
  also in experiment settings, and replaces the domains and hosts in the
  domain mappings. It removes fragments and path parameters from addresses,
  and it redacts more credential and session forms.
