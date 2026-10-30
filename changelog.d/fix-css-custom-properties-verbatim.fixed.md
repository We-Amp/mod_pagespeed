- **`rewrite_css` keeps CSS custom properties exactly as written.** Custom
  property names such as `--Test` and `--test` stay distinct instead of being
  lowercased into one, and their values are no longer minified: `--gap: 0px`
  keeps its unit and `--c: #ff0` keeps its colour notation, so scripts and
  `var()` consumers see what the stylesheet author wrote. Reported in
  https://github.com/apache/incubator-pagespeed-mod/issues/1971 and
  https://github.com/apache/incubator-pagespeed-mod/issues/2066.
