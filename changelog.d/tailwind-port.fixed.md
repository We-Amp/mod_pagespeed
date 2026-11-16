- **Stylesheets that contain a "<" are inlined again on HTML pages.**
  `inline_css` and `prioritize_critical_css` inline them again, and
  `rewrite_css` again minifies an inline `<style>` that holds one, for example
  stylesheets with `@property` syntax descriptors (Tailwind CSS 4) or range
  media queries. This applies to HTML pages that declare their charset as
  UTF-8, windows-125x or ISO-8859-x, in the response headers or in a meta tag
  near the start of the page; other pages, and XHTML, keep such stylesheets
  as they are. Such a stylesheet is inlined only when it is ASCII or valid
  UTF-8. A "<" in a CSS string or comment is written as the CSS escape
  `\3c `, which means the same; a script that reads such a string from a
  custom property sees the escape.
