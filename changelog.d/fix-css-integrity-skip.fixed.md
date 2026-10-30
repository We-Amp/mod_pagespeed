- **Stylesheets and scripts with an `integrity` attribute are no longer changed.**
  `rewrite_css` used to minify such a stylesheet, so the browser refused it and
  the page rendered unstyled; it now leaves it as written. `rewrite_domains`
  no longer moves an `integrity` stylesheet or script to another host, where
  the browser would block it, and `outline_css` and the `@import`-to-link
  conversion no longer turn a `<style integrity>` into a blocked `<link>`.
  `inline_css` and `inline_javascript` no longer inline a stylesheet or script
  that has `integrity`: inlining dropped the integrity check the page asked
  for, so these now stay external files.
