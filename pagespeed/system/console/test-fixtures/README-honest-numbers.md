Fixtures for the honest-numbers tests (src/lib/utils/honest-numbers.test.ts).

- stats-json-idle-vhost.fixture.json: GET /pagespeed_admin/stats_json of a live
  per-vhost console, trimmed to the envelope and the in-place counters. It is
  the 0/0 case: nothing served, nothing fell through per class. Its `host` is
  the value the module reported before the unset ":0" port was dropped.
- message-history-young-buffer.fixture.json: rebuilt from measured times and
  totals from a live console, not captured verbatim. It models the grouped
  message log of a console whose retained buffer spans about four minutes (817
  lines, 16:19:46 to 16:23:37), in the shape of GET
  /pagespeed_admin/message_history?grouped=1. Group counts are reduced to the
  largest kinds and the template text is illustrative.
