- **Critical CSS is inlined again for stylesheets containing a "<".** A
  stylesheet with `@property` syntax descriptors such as `"<length>"` (Tailwind
  CSS 4 emits these) or range media queries such as `@media (width < 600px)`
  was never inlined by `prioritize_critical_css` or `inline_css`. Only a
  closing style tag in the stylesheet keeps it external now, and the debug
  message names that reason.
