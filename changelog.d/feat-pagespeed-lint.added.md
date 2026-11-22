- **New `pagespeed lint` checks an Apache configuration without starting Apache.**
  It reads a configuration file, the `Include` files it pulls in (`--root` sets
  their base directory) and any `.htaccess` files named with `--htaccess`. It
  reports unknown or misspelled directives, invalid values, wrong argument
  counts, unknown and deprecated filter names, retired options, and directives
  in places Apache or the module rejects, each with a rule id, file and line.
  It uses the module's own option parser and makes no network connections.
  Values are shown only for options known to be safe to print; all others
  appear as `<masked>`. Example: `pagespeed lint --root /etc/apache2 apache2.conf`.
  Output is JSON (schema `pagespeed-lint/1`) with `--json` or when piped. Exit
  status is 0 when clean, 1 when findings reach `--fail-on` (default `error`),
  2 for a usage or read error, 3 for an internal error. Download it from the
  release assets as `pagespeed-lint-<version>-linux-x86_64.tar.gz`,
  `-linux-aarch64.tar.gz` or the signed `-win-x64.zip`.
