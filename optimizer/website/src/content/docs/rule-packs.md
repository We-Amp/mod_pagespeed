---
title: 'Rule packs'
description: 'Rule packs for Apache, nginx and IIS: JSON rules for the canonical, title, description, hreflang and JSON-LD of your pages, in report mode first.'
order: 47
group: 'Filters'
lastUpdated: 2026-10-20
---

A rule pack is a JSON file of rules for the head of your HTML pages. The `rule_packs` filter reads it and, where a rule says so, adds a missing canonical link, title, meta description, hreflang cluster or JSON-LD block, and removes duplicates of them. It runs in the same rewriting chain as the other filters, on Apache, nginx and IIS.

:::note[Availability]
The `rule_packs` filter and the free edge-SEO pack are available from mod_pagespeed 2.3.0 (packages 1.18.0). The filter is off by default and is in no rewrite level. See [Rule packs](/packs/) for the packs themselves.
:::

The filter does nothing without a pack file, and a pack starts in report mode: it records what it would change and changes nothing.

## What a pack does and does not do {#what-it-does}

- It handles five kinds of head element: `canonical`, `title`, `description`, `hreflang` and `jsonld`, and, when a rule asks for it, the `Link` response header for the canonical. It does not edit anything else in the page.
- Applying a pack fetches nothing over the network. The pack is data, not code.
- Up to eight pack files per server (virtual host or site): repeat `RulePackFile` for each, and a ninth is a configuration error. Packs run in configuration order, and every pack on a server needs its own id (1 to 64 lowercase letters, digits or `-`); a file whose id an earlier file already uses is refused.
- A host that is not listed in the pack is never touched.
- It is designed not to fail a response. A pack that does not load, a page it cannot parse or edit safely, or an internal error means the page is served as the origin sent it.
- It only sees responses the module rewrites: status 200, HTML, not already marked `X-Mod-Pagespeed`.

Changes are made in the head, with three exceptions: a JSON-LD block the pack adds goes at the end of `<body>`; a late canonical, title, description or hreflang element in the body can be removed (see [Late elements](#late-elements)); and a canonical rule can set the `Link` response header (see [The Link header](#link-header)).

## Enable it {#enable}

`RulePackFile` names a pack file. Repeat it to load more than one pack (at most eight per server or virtual host; two lines naming the same path count once); the packs run in the order of the lines, and the first pack whose rule applies to a kind of element claims it. A virtual host that sets `RulePackFile` uses its own list and ignores the server-wide one; one that sets none inherits it. `RulePackMode` accepts `report` (the default) or `enforce`; any other value is a configuration error.

Both directives are server-scoped. They cannot be set in `.htaccess`, `<Directory>`, an nginx `location` block or an IIS match block. In `.htaccess` Apache answers 500 for that directory; `<Directory>` and nginx `location` fail at configuration load; IIS warns and continues without the setting. `DisableFilters rule_packs` turns the filter off for a directory or location.

### Apache

```apache
<VirtualHost *:443>
  ServerName www.example.com
  ModPagespeedEnableFilters rule_packs
  ModPagespeedRulePackFile /etc/pagespeed/packs/example.com.json
  ModPagespeedRulePackMode report
</VirtualHost>
```

### nginx

In the `server` block:

```nginx
pagespeed EnableFilters rule_packs;
pagespeed RulePackFile /etc/pagespeed/packs/example.com.json;
pagespeed RulePackMode report;
```

### IIS

In the site's own `pagespeed.config`, at the top level, not inside a `host:` match block (the path is an example):

```text
pagespeed EnableFilters rule_packs
pagespeed RulePackFile "C:\pagespeed\packs\example.com.json"
pagespeed RulePackMode report
```

Keep the pack outside the site's web root; the application pool identity needs read access. See [IIS configuration](/docs/iis-configuration/) for the file format.

### Loading and reloading

The serving process reads the pack the first time it needs it, not when the configuration is parsed. A graceful restart or reload picks up a changed file. As a safety net each process checks the file at most every 10 seconds and reloads it when the content changed. If a new file fails to load, the process keeps the last good pack and logs the error.

Enabling the filter on a server changes its cache signature once, so that server's cached resources are rebuilt once.

## Pack format {#pack-format}

A minimal pack:

```json
{
  "pack": { "id": "edge-seo", "version": "0.1.0" },
  "sites": [{ "host": "www.example.com", "mode": "report" }],
  "defaults": { "paths": ["/**"], "exclude_paths": ["/admin/**"] },
  "rules": [
    {
      "id": "canonical-all",
      "kind": "canonical",
      "value": { "template": "https://www.example.com{path}" }
    }
  ]
}
```

`pack`, `sites` and `rules` are required; `defaults` and `tables` are optional. Unknown keys, repeated keys and unknown placeholders are load errors, so a typo cannot quietly become a different rule.

- **`pack`**: `id` (1 to 64 lowercase letters, digits or `-`, unique among the server's packs) and `version` (`MAJOR.MINOR.PATCH`, or the pack fails to load) are required. Optional: `publisher` (default `local`; also the key id of a [signed pack](#signed-packs)), `description`, and `engine_min`, the module version the pack needs; an older module refuses to load it. The top-level key `signature` is reserved and is a load error.
- **`sites`**: a list of `{ "host", "mode" }`. `host` is a lowercase host name or `*.suffix`; `*.example.com` matches subdomains, not `example.com` itself. `mode` is `report` (default) or `enforce`.
- **`defaults`**: `paths` and `exclude_paths` (glob lists), used by a rule that names none of its own. The fallback is per list: a rule's own `exclude_paths` replaces the defaults. Without `defaults`, paths are `/**`.
- **`tables`**: named maps of per-URL values for rules to look up, and the `hreflang` table of language clusters.
- **`rules[]`**: `id` (`[a-z0-9-]`, at most 64 characters, unique), `kind`, `value`, and optional `enabled` (default `true`), `enforce` (default `false`), `match`, `on_present` (default `keep`) and, on canonical rules, `link_header`.
- **`match`**: `paths` and `exclude_paths` (globs) and `path_regex` (a regular expression with captures `{1}` to `{9}`).
- **`value`**: one of `template` (text with placeholders), `table` (the name of a `tables` map, with an optional `fallback_template`), or, for `hreflang`, `pattern` (language code to URL template) or `"table": "hreflang"` to use the cluster table.

### Rule kinds

| `kind`        | What it sets                         |
| ------------- | ------------------------------------ |
| `canonical`   | `<link rel="canonical">`             |
| `title`       | `<title>`                            |
| `description` | `<meta name="description">`          |
| `hreflang`    | The cluster of hreflang links        |
| `jsonld`      | A `<script type="application/ld+json">` block |

For each kind the first enabled rule in file order whose `match` fits the page wins. The loader warns about a rule that an earlier rule of the same kind always shadows. A rule with no value for a page does nothing on that page.

### Placeholders {#placeholders}

Templates can use `{scheme}`, `{host}`, `{path}`, `{query}`, `{url}`, and `{1}` to `{9}` from `path_regex`. In `jsonld` templates only, `{title}`, `{description}` and `{canonical}` give the values the page ends up with.

### Start from the template {#template}

The [packs directory](https://github.com/We-Amp/pagespeed-optimizer/tree/main/packs) of the pagespeed-optimizer repository holds the free [template](https://github.com/We-Amp/pagespeed-optimizer/blob/main/packs/edge-seo/pack.example.json). It lists one placeholder host, `www.example.com`, in report mode, and has five rules, one per kind, each with `on_present` at `keep` and `enforce` at `false`:

- a canonical built from the template `https://www.example.com{path}`;
- a title and a meta description, each from its own table;
- an hreflang cluster from the `hreflang` table;
- an `Organization` JSON-LD block that uses `{canonical}`.

Every host and URL in the template is a placeholder to replace with your own. A host that is not listed in `sites` is never touched, so as shipped the template does nothing on your site.

## Keep, repair, replace {#on-present}

Each rule's `on_present` setting says what to do when the page already has an element of that kind.

| `on_present`     | When it acts                                                                         | What it does to existing elements                                                                                       |
| ---------------- | ------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| `keep` (default) | The page has none                                                                    | Removes exact duplicates. Does not change or remove a different value in the head.                                      |
| `repair`         | None, or the existing ones are unusable, empty, outside the head, or in conflict     | Removes the unusable, empty and out-of-head ones, keeps the first usable one, and adds what is missing.                 |
| `replace`        | Always                                                                               | The page ends with exactly the rule's value.                                                                            |

An element that already equals the target value is never rewritten, so running the filter over its own output changes nothing.

For a canonical rule whose value is `https://www.example.com/a`:

- `keep`: the page has no canonical. The filter adds `<link rel="canonical" href="https://www.example.com/a">` to the head. If the page already had a canonical in the head with another URL, it stays.
- `repair`: the page has two canonicals with different URLs, or one in the body. The filter removes the one in the body, keeps the first usable one in the head, and adds the rule's value only if no usable one is left.
- `replace`: the page has `https://www.example.com/old`. The page ends up with `https://www.example.com/a` and nothing else.

A page without an explicit `<head>`, or whose `<head>` comes after `<body>`, is skipped (`no_head`). Elements inside `svg`, `math`, `template`, `noscript`, `noembed` and `noframes` are ignored.

### When decisions are made {#decisions}

The module works on the page in windows, and an element that has already been sent cannot be changed.

- `canonical`, `title`, `description`, `hreflang` are decided when the head ends: at `</head>`, at `<body>`, at the first element that cannot be head content (such as a `<div>`), or at the end of the document, whichever comes first. New elements are inserted before the first non-head element, or at the end of `<head>`.
- `jsonld` is decided at the end of the document, so a block in the footer counts and a second one is not added next to it. A block belongs to a rule only if its top-level `@type` (including members of a top-level `@graph`) overlaps the rule's. A new block goes at the end of `<body>`, else at the end of `<html>`.

### Late elements {#late-elements}

A late element is one of these kinds that first appears after its decision, typically in the body.

- `canonical`, `title`, `description`: under `repair` and `replace` a late element is removed. Under `keep`, if the pack inserted that element on this page, a late one is removed, because search engines commonly ignore a canonical in the body and leaving it next to the inserted one would give the page two conflicting canonicals; in that one case `keep` acts like `repair`. If the head already had one, `keep` removes only exact duplicates.
- `hreflang`: under `repair` and `replace` a late link is removed. Under `keep` it is removed only when the pack inserted the cluster and the link's language code is in it.
- In report mode, or when the head plan was dropped, nothing is removed and the decision is logged.

### The Link header {#link-header}

A canonical rule can carry `"link_header": "match"` (default `"off"`). The filter then keeps the `Link: <url>; rel="canonical"` response header consistent with the canonical the page ends up with, at the end of the head:

| Origin `Link` header  | `keep`                                                                                      | `repair` / `replace`                                                                                                              |
| --------------------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| No canonical entry    | Adds `<url>; rel="canonical"`, only if the page's final canonical equals the rule's own value | Same                                                                                                                              |
| Entries agree with the page | Nothing                                                                               | Nothing                                                                                                                           |
| An entry differs      | Nothing; logs `header_conflict`                                                             | Removes the conflicting canonical entries, keeps every other entry (preload, preconnect, ...), adds the page's canonical if none is left |

A `keep` page that keeps its own canonical gets no header. A page whose head plan was dropped makes no header decision. Report mode never writes the header. A `Link` line the filter cannot parse completely is left as it is.

The action sees only the headers the module sees when it runs. A `Link` header added later, for example by Apache `Header always set` or by another module that runs after the PageSpeed output filter, is invisible to it and is not de-duplicated. Check the `Link` headers on the wire before you enable the action. The action is skipped once the first part of the response has been sent (`headers_flushed`). On a host with a pack, response headers go out with the first body bytes or at the end of the response, not on an early flush, so an origin that flushes its headers early to start browser preloads loses that head start.

## Report first, then enforce {#report-then-enforce}

Three settings must all say enforce before a rule changes a page: the server directive `RulePackMode`, the matching site's `mode`, and the rule's `enforce`. The lowest of the three wins, and each defaults to report.

1. Write the pack with every `mode` at `report` and every `enforce` at `false`, and [enable the filter](#enable). Restart or reload the server.
2. Browse or crawl the site. Report mode does not change the HTML it serves (with `debug` off, the HTML is the same as without the filter). It writes the [decision log](#decision-log) and moves the [counters](#statistics).
3. Read the decision log. Each line says what a rule would do on a page and why. Look at `defect` and `reason` first.
4. Compare with an external check that fetches the page over HTTP and inspects the HTML and response headers. The log shows what the module saw; the external check shows what was served.
5. Set `enforce: true` on one rule, set its site to `enforce`, and set `RulePackMode enforce`. Reload, re-crawl, compare again. Then the next rule.

### The debug comment {#debug}

To see what the filter did to a page, request it with `?PageSpeedFilters=+debug` (or enable the [`debug`](/docs/html-filters/#debug) filter). It writes a comment before each change:

```html
<!-- pagespeed-pack edge-seo@0.1.0 rule=canonical-all action=insert mode=enforce -->
```

In report mode the comment reads `would=insert`. This is also how to find out why no rule acted on a path: there is no counter for "no rule matched this path".

## Decision log {#decision-log}

The log is written to `<LogDir>/packs/<pack-id>/decisions-YYYY-MM-DD.jsonl`, with a UTC date. `LogDir` must be writable by the server user. On Linux and macOS the files are created with mode 0640 (readers need the server's group); on Windows they inherit the directory's ACL. With an empty `LogDir` there is no log (the counters still work) and one warning is written per process.

One JSON object per line, one line per decision:

| Field     | Content                                                                                    |
| --------- | ------------------------------------------------------------------------------------------ |
| `ts`      | Milliseconds since the epoch                                                               |
| `pack`    | `id@version`                                                                               |
| `sha256`  | First 16 hex digits of the pack file's hash                                                |
| `host`    | The request host                                                                           |
| `path`    | The path with its query, cut at 300 bytes                                                  |
| `rule`, `kind`, `mode` | The rule and its kind, and the effective mode                                 |
| `action`  | `insert`, `fill`, `replace`, `remove`, `dedupe` or `none`                                  |
| `reason`  | Such as `equal`, `present`, `late_conflict`, `not_rewritable`                              |
| `defect`  | Always present, empty when none                                                            |
| `before`  | A hash of the value before                                                                 |
| `after`   | The value after, cut at 200 bytes                                                          |
| `removed` | Present when elements were removed: their hrefs or `@type`s in clear text                  |

The line for the Link header action carries `"header": true`. One rule can log more than one line for a page: the head decision, the late element decision and the header decision.

The log does not record the client address, cookies or user agent. The path is logged with its query string, so treat the log as you would an access log.

A process writes a line at most once per key (host, path, rule, pack hash, mode, action, reason and the header marker). The processes of a server do not coordinate, so several processes can each write the same line, and a process clears its key set at 65,536 keys and can then repeat a line. Count distinct keys, not lines.

All processes together write about 50 MiB per day. The size is checked every 256 lines, so it can overshoot. After that, lines for the rest of the day are dropped. `rule_pack_decision_log_dropped` counts those, and also failed writes and a directory that cannot be created. The newest 14 calendar days are kept. Do not delete or rotate the current day's file: a process that still holds a deleted file keeps writing into it and those lines are lost.

## Statistics and the admin console {#statistics}

The counters are on the statistics page, per virtual host with `UsePerVHostStatistics on`. See [the admin console](/docs/admin-console/).

| Group           | Counters                                                                                                                                                                          |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Load            | `rule_pack_loads`, `rule_pack_load_errors`                                                                                                                                        |
| Signatures      | `rule_pack_loads_signed`, `rule_pack_refused_unsigned`, `rule_pack_refused_signature_unreadable`, `rule_pack_refused_publisher_mismatch`, `rule_pack_refused_unknown_key`, `rule_pack_refused_bad_signature` |
| Pages           | `rule_pack_pages_evaluated`, `rule_pack_pages_modified`, `rule_pack_pages_would_modify`                                                                                           |
| Pages skipped   | `rule_pack_pages_skipped_host_not_listed`, `rule_pack_pages_skipped_no_head`, `rule_pack_pages_skipped_malformed_head`, `rule_pack_pages_skipped_not_rewritable`, `rule_pack_pages_skipped_size_limit` |
| Decisions       | `rule_pack_decisions_applied`, `rule_pack_decisions_would_apply`                                                                                                                  |
| Late elements   | `rule_pack_late_removed`, `rule_pack_late_unremovable`                                                                                                                            |
| Link header     | `rule_pack_headers_set`, `rule_pack_headers_flushed` (move only with the Link header action)                                                                                      |
| Log             | `rule_pack_decision_log_lines`, `rule_pack_decision_log_dropped`                                                                                                                  |

`rule_pack_pages_skipped_error` is reserved and always 0. There are no per-rule counters; per-rule numbers come from the decision log.

The admin console also has a Rule packs page. It does not show signature results, and a pack refused by the signature check still shows as loaded there; for those, use the server log and the counters above.

## Limits {#limits}

A pack that exceeds a load limit does not load.

| Load limit                 | Value                         |
| -------------------------- | ----------------------------- |
| Pack file size             | 1 MiB                         |
| Rules                      | 200                           |
| Table entries (all tables) | 50,000                        |
| Template size              | 16 KiB                        |
| Distinct path patterns     | 2,000 (32 per list in a rule) |
| Path pattern               | 512 bytes                     |
| Regular expression         | 1,024 bytes, 1 MiB of memory  |
| Rule id                    | 64 characters                 |
| Host                       | 253 characters                |
| JSON nesting in the pack   | 32 levels                     |
| JSON-LD template nesting   | 64 levels                     |

Literal table values are checked at load, so an over-long title in a table is a load error. Templates are checked when the page is built: at page time, a value over these limits is not applied and the decision is `invalid_value`:

| Page-time limit  | Value            |
| ---------------- | ---------------- |
| URL              | 2,048 bytes      |
| Title            | 300 characters   |
| Description      | 1,000 characters |
| Expanded JSON-LD | 16 KiB           |

The added-bytes budget is 64 KiB per page, shared by the head and the end-of-document changes; it counts 256 bytes per change more when `debug` is on. If JSON-LD overflows it, only the JSON-LD change is dropped (`size_limit`) and the head changes stay. A JSON-LD block on the page over 1 MiB, or nested deeper than 64 levels, is not parsed, and the rule then leaves the page's JSON-LD alone.

## Signed packs {#signed-packs}

Signing is optional and off by default: without the settings below, every pack that loads is used.

A signed pack carries an Ed25519 signature over the exact bytes of the pack file, in a sidecar next to it named after it with `.sig` added (`example.com.json.sig`). If the pack path is a symlink, the sidecar sits next to the file the link points to. The sidecar is one line, at most 1,024 bytes:

```text
ed25519 <key id> <base64 signature>
```

The key id is lowercase letters, digits and `-` (1 to 64 characters) and must equal the pack's `publisher`.

- `RulePackTrustKey <key id> <base64 public key>` lists a key the server trusts. The key is 44 base64 characters. Repeat the directive for up to 16 keys per server or virtual host. A virtual host adds to the server's keys, and a virtual host key with the same id replaces the server's. The We-Amp publisher key (key id `we-amp`) is built in, so packs it signed verify without a `RulePackTrustKey` line; a key you list with the id `we-amp` replaces it.
- `RulePackRequireSigned on|off` (default `off`) decides what happens to a pack that fails the check.

The server runs these checks in order; the first one that fails names the reason:

1. `unsigned`: there is no `.sig` file.
2. `signature unreadable`: the sidecar cannot be read or is not in the format above.
3. `publisher mismatch`: the key id in the sidecar is not the pack's `publisher`.
4. `unknown key`: no `RulePackTrustKey` (and not the built-in `we-amp` key) for that key id.
5. `bad signature`: the signature does not match the pack's bytes and that key.

A pack that passes all five is verified. With `RulePackRequireSigned off`, every pack that loads is used: a verified pack logs `rule pack loaded signed`, any other pack logs the `rule pack loaded unsigned` warning, and only when a `.sig` file exists but fails one of the checks 2 to 5 does a second warning give the reason. With `on`, a pack that fails any check is refused: an earlier accepted version of the same file stays in use, otherwise the filter is off for that server or virtual host.

Both directives are server-scoped, like `RulePackFile`, so a per-directory configuration can neither add a key nor relax the requirement. On Apache they carry the `ModPagespeed` prefix; on IIS they go at the top level of `pagespeed.config`.

```nginx
pagespeed RulePackTrustKey example-publisher AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=;
pagespeed RulePackRequireSigned on;
```

The key above is an example, not a real key. The signing tool is `tools/rule-packs/sign-pack.sh` in the mod_pagespeed repository, published with the release; it creates keys, prints the `RulePackTrustKey` line, signs a pack and verifies one. Keep the private key off the servers and out of any repository. Sign after the last edit: any change to the pack, even a line ending, needs a new signature.

## Operating notes {#operating}

- **Turn it off**: remove the directive and reload. Deleting the pack file does not turn it off; the process keeps the loaded pack and logs that the file cannot be read.
- **After a pack change**, processes can disagree for up to 10 seconds. A new process that finds a bad file has no pack, while older ones keep the good one.
- **Caches in front**: if a CDN or browser caches your HTML, a pack change reaches visitors only as those copies expire.
- **With the optimizer worker**: on Apache, nginx and IIS the worker serves no HTML. Every HTML response is rewritten by the module's own filter chain, which carries the pack; `ipro_daemon_fallthrough` rises on HTML requests as part of this normal behavior. CSS, JavaScript and images are served by the worker as before.

## Troubleshooting {#troubleshooting}

**Nothing happens.** Check, in order:

1. The filter is enabled and `RulePackFile` is set (otherwise the debug output says "no rule pack file").
2. The pack loaded (otherwise "no usable rule pack").
3. The host is listed in `sites` (`rule_pack_pages_skipped_host_not_listed`).
4. A rule's paths match and are not excluded. There is no counter for this; use `?PageSpeedFilters=+debug`.
5. The page has a well-formed head (`no_head`, `malformed_head`). This is only evaluated once a rule matched.
6. The mode is not still report at one of the three levels.

**The pack did not load.** `rule_pack_load_errors` goes up and the error log has a warning, `rule pack <path> (sha256=<64 hex>) failed to load: <path>: <json path>: <reason>`, followed by `keeping the pack loaded earlier` or, when there was no earlier pack, `the rule_packs filter is off for it`.

**`not_rewritable`.** A flush split the head and the part the rule has to change was already sent. The filter then drops all head changes for that page and leaves the head as the origin sent it. Inserts into a head that is still open still work.

**`headers_flushed`.** The first part of the response left before the head ended, so the `Link` header could no longer be changed. Other changes are not affected.

**`skipped_malformed_head`.** An inert element (for example `svg`) was left unclosed in the head and swallowed the rest of it, so the page was skipped.
