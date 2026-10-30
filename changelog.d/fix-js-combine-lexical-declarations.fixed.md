- **`combine_javascript` no longer breaks scripts that declare top-level `const`,
  `let` or `class`.** The combined scripts run through `eval()`, which kept
  those declarations private, so a later script could no longer see a
  function declared as `const checkForm = ...`. Such scripts are now left
  uncombined. Thanks to the original reporter:
  https://github.com/apache/incubator-pagespeed-mod/issues/1887
