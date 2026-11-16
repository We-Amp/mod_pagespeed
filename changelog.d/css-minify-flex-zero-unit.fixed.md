- CSS minification no longer rewrites `flex: 1 0px` to `flex: 1 0`, which
  changed the shrink factor and the flex basis. Zero units are now kept
  inside the `flex` shorthand.
