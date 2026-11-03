- A `LoadFromFileMatch` replacement that contains `/./` or `/../` no longer loads from file; the resource is fetched over HTTP instead.
  File-load rules and mapped file names are now compared in normalised form; rules written with single slashes are unaffected.
