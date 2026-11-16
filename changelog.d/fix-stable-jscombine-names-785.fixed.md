- **Combined scripts keep running when an input is served unoptimized.** A
  combined JavaScript file declares each script under a name the page then
  evaluates. When a script inside the combination was itself a minified
  script (as in pages rewritten by older versions) and could only be served
  unminified, or had changed since the page was rewritten, the combined file
  declared it under another name and the page's scripts failed with a
  `ReferenceError`. The combined file now always uses the name the page
  expects.
