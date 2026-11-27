# Exclusions

Inputs excluded from the cross-product goldens, per SPEC.md §5. Each entry
records the input path, the nature of the divergence, and the reason it is
excluded rather than fixed.

Format convention (parsed by the differential gates): one bullet per
input, leading with the backtick-quoted manifest path.

(No exclusions currently.)

Note: `seeds/doctype-21-lowercased-fpi.html` was previously excluded
(1.15: `UNKNOWN`; 2.0: `XHTML_1_0_STRICT`) because 2.0 matched the FPI
ASCII case-insensitively while 1.15 compared FPI bytes exactly. Resolved:
canonical `pagespeed/kernel/html/doctype.cc` now matches the FPI ASCII
case-insensitively too (browser behavior;
deviation 1 ported upstream), so both probes classify it
`XHTML_1_0_STRICT`.

Note: `seeds/doctype-15-html5-legacy-compat.html` was expected to diverge
(deviation 2, `about:legacy-compat`) but does NOT under the corpus's fixed
`text/html` session — both probes classify it `HTML_5`. No exclusion
needed.
