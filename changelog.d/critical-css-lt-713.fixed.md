- **Critical CSS is inlined again for HTML pages whose stylesheet contains a
  "<".** On HTML pages, a stylesheet with `@property` syntax descriptors such
  as `"<length>"` (Tailwind CSS 4 emits these) or range media queries such as
  `@media (width < 600px)` was never inlined by `prioritize_critical_css` or
  `inline_css`, and an inline `<style>` with such rules was not minified. Only
  a closing style tag in the stylesheet keeps it external now, and the debug
  message names that reason. XHTML pages keep the previous, more conservative
  behaviour.
