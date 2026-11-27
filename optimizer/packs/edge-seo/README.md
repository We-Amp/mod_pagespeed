# The edge-SEO pack

`pack.example.json` is the free edge-SEO rule pack for mod_pagespeed. It is a
template: every host, URL and text in it is an `example.com` placeholder that
you replace with your own before the pack does anything useful.

The pack holds one worked rule for each kind of element the `rule_packs`
filter handles: the head elements (the canonical link, the title, the meta
description and the hreflang cluster) and the JSON-LD block. It needs mod_pagespeed 1.18.0 or newer
(`engine_min`); an older module refuses to load it.

## What it is and is not

- It is data, not code. The module reads the file from disk; applying it
  fetches nothing over the network, and the server does not contact anyone to
  use it.
- It starts in report mode. The site is listed with `"mode": "report"` and every
  rule has `"enforce": false`, so the module records what each rule would
  change and serves the HTML unchanged.
- It edits only the head elements and the JSON-LD block named above (and, for
  a canonical rule with `link_header`, the `Link` response header). The module
  adds a new JSON-LD block at the end of the body. A canonical, title,
  description or hreflang element that appears late, in the body, can be
  removed too (see below). It does not touch the rest of the page, your CSS,
  scripts or images.
- It acts only on the hosts listed in `sites`. The template lists
  `www.example.com`, so as shipped it does nothing on your site.
- Every rule uses `on_present: keep`: it adds an element only when the page has
  none, and otherwise removes only exact duplicates. A value your page already
  sets in its head stays. The one exception is a late element: when the pack
  inserted a canonical, title or description on a page, a late one of the same
  kind in the body is removed, and when it inserted an hreflang cluster, a late
  hreflang link for a language in that cluster is removed. In report mode
  nothing is removed; the decision is only logged.
- It is not signed. Signing is for packs a publisher distributes for you to use
  as they are; a template you edit gets a new hash with every change, so a
  signature on it would not survive your first edit. The server logs a warning
  that the pack is unsigned when it loads it, which is expected here. With
  `RulePackRequireSigned on` this unsigned template is refused, so leave that
  directive off (its default) for the free pack.

## Adapting it

Copy the file outside your web root, for example to
`/etc/pagespeed/packs/example.com.json`, and edit the copy. The loader is strict:
an unknown key, a repeated key or an unknown `{placeholder}` stops the pack from
loading, and the error log names the JSON path that is wrong.

### `pack`

Keep `id` as `edge-seo`; the loader accepts no other id. Raise `version`
(`MAJOR.MINOR.PATCH`) when you change the file, so the decision log and the
debug comments show which version made a decision. Do not add a top-level
`signature` key; it is reserved and is a load error.

### `sites`

List each host the pack applies to, in lowercase, without a port:

```json
"sites": [
  { "host": "www.your-site.example", "mode": "report" },
  { "host": "*.shop.your-site.example", "mode": "report" }
]
```

`*.suffix` matches the subdomains of `suffix`, not `suffix` itself, and needs at
least two labels after `*.`; a bare `*` is not accepted. A host that is not
listed is never touched. `mode` is `report` (the default) or `enforce`.

### `defaults`

`paths` and `exclude_paths` are path globs used by every rule that names none of
its own. `*` matches within one path segment, `**` matches across segments, and
`/shop/**` matches `/shop/` and below but not `/shop`. A rule's own
`exclude_paths` replaces the default list rather than adding to it.

### `tables`

Named maps from a URL path (optionally with `?query`) to a value, plus the
`hreflang` table of language clusters. A rule that reads a table does nothing on
a page that has no entry in it.

### The rules

Each rule has an `id` (`a-z`, `0-9` and `-`, up to 64 characters), a `kind` and
a `value`, and optionally `enabled`, `enforce`, `match` (`paths`,
`exclude_paths`, `path_regex`) and `on_present` (`keep`, `repair` or
`replace`). For each kind, the first enabled rule whose `match` fits the page
wins.

- `canonical-all` (`canonical`) builds the canonical URL from a template:
  `https://www.example.com{path}`. Replace the scheme and host with your
  canonical origin. `{path}` is the request path without the query, so query
  string variants of a page point at one canonical URL. Other placeholders:
  `{scheme}`, `{host}`, `{query}`, `{url}`, and `{1}` to `{9}` from a
  `path_regex`. To also keep the `Link: <url>; rel="canonical"` response
  header in line with the page, add `"link_header": "match"` to this rule.
- `title-from-table` (`title`) takes the title from the `titles` table. Add one
  entry per page you want to fix; a page without an entry is left alone. A
  title in the table must be 1 to 300 characters, or the pack does not load.
- `description-from-table` (`description`) takes the meta description from the
  `descriptions` table, the same way. A description in the table must be 1 to
  1,000 characters.
- `hreflang-cluster` (`hreflang`) inserts the language cluster a page belongs
  to, from the `hreflang` table. Each cluster maps a language code (or
  `x-default`) to an absolute URL, a URL belongs to at most one cluster, and a
  cluster is only written on a page that is itself one of its URLs. The rule's
  `match.paths` lists the pages of the cluster. For a site with one language
  tree per prefix, a rule with a `path_regex` and a `pattern` (language code to
  URL template) replaces the table.
- `jsonld-organization` (`jsonld`) adds a schema.org `Organization` block to the
  home page. The value is a JSON template; `{canonical}`, `{title}` and
  `{description}` give the values the page ends up with. Under `keep` the rule
  adds the block only when the page has no JSON-LD block of the same `@type`.

Set `"enabled": false` on a rule you do not want instead of deleting it, so it
is easy to turn on later.

## Enabling it

`RulePackFile` and `RulePackMode` are server-scoped: set them for a server,
virtual host or site, not in `.htaccess`, `<Directory>`, an nginx `location`
block or an IIS match block.

Apache:

```apache
<VirtualHost *:443>
  ServerName www.example.com
  ModPagespeedEnableFilters rule_packs
  ModPagespeedRulePackFile /etc/pagespeed/packs/example.com.json
  ModPagespeedRulePackMode report
</VirtualHost>
```

nginx (in the `server` block):

```nginx
pagespeed EnableFilters rule_packs;
pagespeed RulePackFile /etc/pagespeed/packs/example.com.json;
pagespeed RulePackMode report;
```

IIS (at the top level of the site's own `pagespeed.config`, not inside a
`host:` match block; keep the pack outside the web root and give the
application pool identity read access):

```
pagespeed EnableFilters rule_packs
pagespeed RulePackFile "C:\pagespeed\packs\example.com.json"
pagespeed RulePackMode report
```

Reload or restart the server. The module reads the pack the first time it
needs it, and checks the file for changes at most every 10 seconds. If a
changed file fails to load, the module keeps the last good pack and logs the
error.

## Seeing what it would do

- The admin site has a Rule packs page (`/pagespeed_admin/rule_packs` by
  default) that shows the configuration, the loaded pack, the counters and the
  decisions logged today.
- The decision log is written to `<LogDir>/packs/edge-seo/decisions-YYYY-MM-DD.jsonl`,
  one JSON line per decision: the host, path, rule, mode, the `action` it took or
  would take, and the `reason`. `LogDir` must be writable by the server user.
- Requesting a page with `?PageSpeedFilters=+debug` adds an HTML comment before
  each change, such as `<!-- pagespeed-pack edge-seo@1.0.0 rule=canonical-all
  action=insert mode=enforce -->`; in report mode it reads `would=insert`. This
  is also how to find out why no rule acted on a path.
- The statistics page counts loads, load errors, and pages evaluated, modified
  and that would be modified (`rule_pack_*`).

## Turning it on

Three settings must all say enforce before a rule changes a page: the
`RulePackMode` directive, the site's `mode` in the pack, and the rule's
`enforce`. Move one rule at a time: set its `enforce` to `true`, set the site
to `enforce` and the directive to `enforce`, reload, and check the pages again.
A JSON-LD rule that quotes `{title}`, `{description}` or `{canonical}` should be
turned on together with the rules it quotes.

To turn the pack off, remove the `RulePackFile` directive and reload; deleting
the file alone keeps the last loaded pack in use.

## The full reference

The operator guide is published at <https://modpagespeed.com/docs/rule-packs/>.
It describes every field, the limits, the decision log format, the statistics,
signed packs and troubleshooting.

The pack is free to use and to change.
