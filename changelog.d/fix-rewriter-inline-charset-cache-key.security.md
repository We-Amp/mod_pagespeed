- Corrected a regression in a 2.5.0 hardening change that could serve a small
  inline script to a page in a different character set than the one it was
  checked in, so that on sites that mix character sets, page text could be read
  as script. Affected: 2.5.0 to 2.6.0. Update recommended for sites with pages
  in more than one character set.
