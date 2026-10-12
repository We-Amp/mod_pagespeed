# edge-seo fixtures

Each directory under `fixtures/` is one case. The kernel runner
(`test/pagespeed/kernel/packs/fixture_runner_test.cc`) and the chain-level test
run every case.

| File | Meaning |
|------|---------|
| `input.html` | The origin's HTML |
| `request.json` | The request (below) |
| `expected.html`, `expected.decisions.jsonl` | The result, shared with other engines that run the same packs; never edit these for a case that exists elsewhere |
| `expected.module.html`, `expected.module.decisions.jsonl` | Where this module deliberately differs; the case must be listed with its reason in `ModuleOverrides()` |
| `expected.headers` | The response headers after the run, one `Name: value` line each, in order. Required when `request.json` has `origin_headers` |

`request.json` keys:

| Key | Meaning |
|-----|---------|
| `url` | The page URL (required) |
| `mode` | The directive mode, `enforce` (default) or `report` |
| `debug` | `true` turns the debug comments on |
| `pack` | A pack file next to `pack.json`; default `pack.json`. The shared `pack.json` is not edited for module-only rules, they live in their own pack file (`pack.link-header.json`) |
| `origin_headers` | The response headers the origin sent, as `[["Name", "value"], ...]`, in order. Without it the host has no response headers and a `link_header` rule does nothing |
| `flush_every` | Flush the parser every N bytes in every run of the case, so the response headers are gone before the head closes |

Response-header cases (`link-header-*`) run through the same properties as the
others. In addition: report mode must leave the headers as the origin sent
them, the second pass runs on the first pass's output and headers and must
change nothing, and in a chunked run the headers are either the whole-document
ones or the origin's, with the `Link` decision saying `headers_flushed`.

New cases that exist only in this module are written with
`bazel run //test/pagespeed/kernel/packs:fixture_update -- --new <case>`
(review the diff; it refuses a case of the shared `pack.json` that already has
an `expected.html`); overrides of shared cases without `--new`.

## Chunked outcomes

Every case also runs with the parser flushed at several offsets. The head and
the end of the document (JSON-LD) are decided one after the other, each applied
whole or dropped whole, so a chunked run gives one of these, checked exactly:

- the whole-document result;
- the end of the document dropped while the head applied: the page is the
  whole-document run of the pack without the end-of-document rules, and each
  end-of-document decision that would have acted says `not_rewritable`;
- the head dropped while the end of the document applied: the page is the
  whole-document run of the pack without the head rules, every head decision
  that would have acted says `not_rewritable`, and the end-of-document
  decisions are the whole-document ones;
- the appended JSON-LD blocks in front of `</html>` instead of `</body>` when
  `</body>` was already sent: the whole-document page with the blocks moved,
  or the head-dropped page with the blocks moved (not the end-of-document-dropped
  one, which has no blocks);
- nothing applied: the page without the filter, every acting decision
  `not_rewritable`.

`chunked-head-dropped-jsonld-applies` is the case for the third outcome: a
flush right before `</head>` has already sent the `<title>` the head rule
replaces, so the head plan drops while the JSON-LD block is still appended.
`chunked-head-dropped-jsonld-quotes-title` adds a block whose template quotes
`{title}`: with no head plan the planner quotes the page's own (old) title, so
the block carries the old title while the whole-document run carries the new.
