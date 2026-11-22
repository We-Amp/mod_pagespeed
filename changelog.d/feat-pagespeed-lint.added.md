- **`pagespeed lint` checks an Apache configuration without starting Apache.**
  A new standalone tool, published with each release as
  `pagespeed-lint-<version>-linux-x86_64.tar.gz`, `-linux-aarch64.tar.gz` and
  a signed `-win-x64.zip`, reads your configuration file with its `Include`
  files (`--root` sets the directory they are under) and any `.htaccess`
  files you name with `--htaccess`. It reports misspelled or unknown
  directives, invalid values, wrong argument counts, unknown and deprecated
  filter names, retired options, and directives placed where Apache or the
  module refuses them, each with a rule id, file and line. It uses the
  module's own option parser and never connects to the network. Option values
  are printed only for options known to be safe to show; anything that could
  hold a key, token or password is shown as `<masked>`. Run
  `pagespeed lint --root /etc/apache2 apache2.conf`; add `--json` for
  machine-readable output. The exit status is 1 when it finds an error.
