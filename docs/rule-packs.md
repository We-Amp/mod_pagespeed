# Rule packs

Status: the `rule_packs` filter is off by default and in no rewrite level; this
guide describes how to turn it on.

A rule pack is a JSON file of rules for the head of your HTML pages. The
`rule_packs` filter reads it and, where a rule says so, adds a missing
canonical link, title, meta description, hreflang cluster or JSON-LD block, and
removes duplicates of them. It runs in the same rewriting chain as the other
filters, on Apache, nginx and IIS.

It does nothing without a pack file, and a pack starts in report mode, which
records what it would change and changes nothing.

## What a pack does and does not do

- It handles five kinds of head element: `canonical`, `title`, `description`,
  `hreflang` and `jsonld` (and, when asked, the `Link` response header for the
  canonical). It does not edit anything else in the page.
- A server (virtual host or site) can load one pack file or several, up to
  eight; see [Several packs on one server](#several-packs-on-one-server). A
  pack id is 1 to 64 lowercase letters, digits or `-`, and is unique on the
  server.
- The pack is data, not code; applying it fetches nothing over the network. A
  pack can be signed, but does not have to be; see [Signed packs](#signed-packs).
- It is designed not to fail a response: a pack that does not load, a page it
  cannot parse or edit safely, or an internal error means the page is served
  as the origin sent it.

## The ladder: keep, repair, replace

Each rule has an `on_present` setting that says what to do when the page already
has an element of that kind.

| `on_present` | When it acts | What it does to existing elements |
|---|---|---|
| `keep` (default) | The page has none | Removes exact duplicates. Does not change or remove a different value in the head; see the late-element rule for the one exception in the body. |
| `repair` | None, or the existing ones are unusable, empty, outside the head, or in conflict | Removes the unusable, empty and out-of-head ones, keeps the first usable one, and adds what is missing. |
| `replace` | Always | The page ends with exactly the rule's value. |

An element that already equals the target value is never rewritten, so running
the filter over its own output changes nothing.

Examples for a canonical rule whose value is `https://www.example.com/a`:

- `keep`: the page has no canonical. The filter adds
  `<link rel="canonical" href="https://www.example.com/a">` to the head. If the
  page already had a canonical in the head with another URL, it stays.
- `repair`: the page has two canonicals with different URLs, or one in the body.
  The filter removes the one in the body, keeps the first usable one in the head,
  and adds the rule's value only if no usable one is left.
- `replace`: the page has `https://www.example.com/old`. The page ends up with
  `https://www.example.com/a` and nothing else.

A page without an explicit `<head>`, or whose `<head>` comes after `<body>`, is
skipped (`no_head`). Elements inside `svg`, `math`, `template`, `noscript`,
`noembed` and `noframes` are ignored.

## Pack format

A minimal pack:

```json
{
  "pack": { "id": "edge-seo", "version": "0.1.0" },
  "sites": [ { "host": "www.example.com", "mode": "report" } ],
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

Top-level keys: `pack`, `sites` and `rules` are required; `defaults` and
`tables` are optional. Unknown keys, repeated keys and unknown placeholders are
load errors, so a typo cannot quietly become a different rule.

- `pack`: `id` (`[a-z0-9-]`, 1 to 64 characters, unique among the packs of one
  server; it names the pack's log directory) and `version` (`MAJOR.MINOR.PATCH`, or the pack fails to load) are required. Optional:
  `publisher` (default `local`), `description`, and `engine_min`, a module
  version the pack needs; an older module refuses to load it. `publisher` is
  also the key id a signed pack is signed with (see [Signed packs](#signed-packs)).
  The `pack` object may also carry a `license` object with an `entitlement` name
  (`a-z`, `0-9` and `-`, up to 64 characters), an `expires` date (`YYYY-MM-DD`)
  and a `terms` URL (`https://`, up to 512 characters). The module checks their
  format and writes them to the server log at info level, once per pack version
  per server process, when that version is first used; they do not change what
  the pack does. The top-level key `signature` is
  reserved and is a load error.
- `sites`: a list of `{ "host", "mode" }`. `host` is a lowercase host name or
  `*.suffix`; `*.example.com` matches subdomains, not `example.com` itself.
  `mode` is `report` (default) or `enforce`. A host that is not listed is
  never touched.
- `defaults`: `paths` and `exclude_paths` (glob lists), used by a rule that
  names none of its own. The fallback is per list: a rule's own `exclude_paths`
  replaces the defaults. Without `defaults`, paths are `/**`.
- `tables`: named maps of per-URL values for rules to look up, and the
  `hreflang` table of language clusters.
- `rules[]`: `id` (`[a-z0-9-]`, at most 64 characters, unique), `kind`, `value`,
  and optional `enabled` (default true), `enforce` (default false), `match`,
  `on_present` (default `keep`) and, on canonical rules, `link_header`.
- `match`: `paths` and `exclude_paths` (globs) and `path_regex` (a regular
  expression with captures `{1}` to `{9}`).
- `value`: one of `template` (text with placeholders), `table` (the name of a
  `tables` map, with an optional `fallback_template`), or, for `hreflang`,
  `pattern` (language code to URL template) or `"table": "hreflang"` to use the
  cluster table.
- Placeholders: `{scheme}`, `{host}`, `{path}`, `{query}`, `{url}`, and
  `{1}` to `{9}` from `path_regex`. In `jsonld` templates only, `{title}`,
  `{description}` and `{canonical}` give the values the page ends up with.

For each kind the first enabled rule in file order whose `match` fits the page
wins. The loader warns about a rule that an earlier rule of the same kind
always shadows. A rule with no value for a page does nothing on that page.

Three settings must all say enforce before a rule changes a page: the server
directive `RulePackMode`, the matching site's `mode`, and the rule's `enforce`.
The lowest of the three wins, and each defaults to report.

## Report first, then enforce

1. Write the pack with every `mode` at `report` and every `enforce` at `false`,
   and enable the filter (next section). Restart or reload the server.
2. Browse or crawl the site. Report mode does not change the HTML it serves
   (with `debug` off, the HTML is the same as without the filter). It writes the decision log and moves the counters.
3. Read the decision log (below). Each line says what a rule would do on a page
   and why. Look at `defect` and `reason` first.
4. Compare with an external check that fetches the page over HTTP and inspects
   the HTML and response headers. The log shows what the module saw; the
   external check shows what was actually served over HTTP.
5. Set `enforce: true` on one rule, set its site to `enforce`, and set
   `RulePackMode enforce`. Reload, re-crawl, compare again. Then the next rule.

To see what the filter did to a page, request it with `?PageSpeedFilters=+debug`
(or enable the `debug` filter). It writes a comment before each change, such as
`<!-- pagespeed-pack edge-seo@0.1.0 rule=canonical-all action=insert
mode=enforce -->`; in report mode the comment reads `would=insert`. This is also
the way to find out why no rule acted on a path: there is no counter for "no
rule matched this path".

## Directives

`RulePackFile` and `RulePackMode` are server-scoped. They cannot be set in
`.htaccess`, `<Directory>`, an nginx `location` block or an IIS match block.
In `.htaccess` Apache answers 500 for that directory; `<Directory>` and nginx
`location` fail at configuration load; IIS warns and continues without the
setting. `RulePackMode` accepts `report` (default) or `enforce`; any other value
is a configuration error. `DisableFilters rule_packs` turns the filter off for a
directory or location. Enabling the filter on a server changes its cache
signature once, so that server's cached resources are rebuilt once.

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

IIS (in the site's own `pagespeed.config`, at the top level; the path is an
example):

```
pagespeed EnableFilters rule_packs
pagespeed RulePackFile "C:\pagespeed\packs\example.com.json"
pagespeed RulePackMode report
```

On IIS, set the options at the top level of `pagespeed.config`, not inside a
`host:` match block. Keep the pack outside the site's web root; the application
pool identity needs read access.

To load more than one pack, repeat `RulePackFile`; see the next section.

The serving process reads the pack the first time it needs it, not when the
configuration is parsed. A graceful restart or reload picks up a changed file.
As a safety net a process that needs the pack checks the file again once 10
seconds have passed since its last check, and reloads it when the content
changed, so processes can disagree for up to 10 seconds. If a new file fails to
load, the process keeps the last good pack and logs the error.

## Several packs on one server

Repeat `RulePackFile` to load more than one pack on a server. Each line adds
one file:

```apache
<VirtualHost *:443>
  ServerName www.example.com
  ModPagespeedEnableFilters rule_packs
  ModPagespeedRulePackFile /etc/pagespeed/packs/seo.json
  ModPagespeedRulePackFile /etc/pagespeed/packs/extras.json
  ModPagespeedRulePackMode report
</VirtualHost>
```

nginx and IIS take the same repeated line (`pagespeed RulePackFile ...`).

- **Limits.** At most eight files per configuration scope; a ninth is a
  configuration error. Two lines that name the same path (after `.`, `..` and
  repeated separators are resolved) count once. A link to a file is not
  resolved when the configuration is read; it is caught by the duplicate id
  rule below.
- **Scope.** A virtual host that sets `RulePackFile` at least once uses its own
  list and ignores the server-wide one. A virtual host that sets none inherits
  the server-wide list.
- **Order.** The packs load and run in the order of the lines. The decision
  log, the admin page and the claims below all use this order.
- **Ids.** Every pack on a server needs its own id. A file whose id is already
  used by an earlier file is refused: it logs `rule pack refused (duplicate
  id)` once per process and file version, and counts
  `rule_pack_refused_duplicate_id`. The earlier pack keeps working.
- **Each pack decides alone.** A pack applies its own `sites`, `defaults` and
  modes, and reads the page as the origin sent it. A pack does not see what an
  earlier pack inserted.
- **Claims: the first pack wins.** When a pack's rule is selected for a page
  (its site lists the host and the rule's paths match), the pack claims that
  kind of element on the page. For `link_header: match` rules it also claims
  the `Link` header. A later pack's rule of a claimed kind changes nothing and
  logs action `none` with reason `claimed` and `claimed_by`, the id of the
  owner (see [Decision log](#decision-log)). Claims are per page and do not
  depend on modes or on how the response is chunked. A pack never claims
  against itself: two rules of one kind in one pack behave as before.
- **Report mode still owns.** A pack in report mode keeps a later pack in
  enforce mode off the same kind. When two packs carry rules of one kind and
  their modes differ, in either order, the server logs one warning per
  process:
  `rule pack overlap: <kind> owned by <first id> (report) before <later id>
  (enforce); the later pack will not act on it`. The admin page and the JSON
  status show the warning. List the pack you want to act first, or move the
  kind out of the other pack.
- **Empty value.** `RulePackFile` with an empty value clears the inherited list.
- **Repeated lines.** Earlier releases used only the last `RulePackFile` line
  of a scope. Now every line is kept, so check your configuration for a stray
  second line before you upgrade.
- **Envoy.** An Envoy virtual host takes one `RulePackFile`. Put the rules of
  that host in one pack file. Apache, nginx and IIS take the full list.

### Upgrading

A configuration written for a single pack needs no change. One `RulePackFile`
line is a list of one; the pack, its id (including `edge-seo`), the decision
log path and line format, the counters and the admin page's `?raw=1` output are
as they were. A host with one pack never logs `claimed`.

### Page counters with two packs

The page counters (`rule_pack_pages_evaluated`, `..._modified`,
`..._would_modify` and the skipped ones) move once for each pack that
evaluated the page. Take a page with no canonical and no title, in enforce
mode. Pack A has a canonical rule. Pack B has a title rule and a canonical
rule.

| Counter | Change for this page |
|---|---|
| `rule_pack_pages_evaluated` | +2 (A and B) |
| `rule_pack_pages_modified` | +2 |
| `rule_pack_decisions_applied` | +2 (A's canonical, B's title) |
| `rule_pack_decisions_claimed` | +1 (B's canonical, owned by A) |

Now take a pack C, listed after A, whose only rule is a canonical rule. C has
nothing left to evaluate on that page: it adds no page counters, only one
`rule_pack_decisions_claimed` and one `claimed` line in its own log. For
per-pack numbers, count the lines in each pack's decision log.

## When decisions are made, per kind

The module works on the page in windows, and an element that has already been
sent cannot be changed.

- `canonical`, `title`, `description`, `hreflang`: decided when the head ends,
  that is at `</head>`, at `<body>`, at the first element that cannot be head
  content (such as a `<div>`), or at the end of the document, whichever comes
  first. New elements are inserted before the first non-head element, or at the
  end of `<head>`.
- `jsonld`: decided at the end of the document, so a block in the footer counts
  and a second one is not added next to it. A block belongs to a rule only if
  its top-level `@type` (including members of a top-level `@graph`) overlaps the
  rule's. A new block goes at the end of `<body>`, else at the end of `<html>`.

### Late elements

A late element is one of these kinds that first appears after its decision,
typically in the body.

- `canonical`, `title`, `description`: under `repair` and `replace` a late
  element is removed. Under `keep`, if the pack inserted that element on this
  page, a late one is removed, because search engines commonly ignore a
  canonical in the body, and leaving it next to the inserted one would give the
  page two conflicting canonicals. So in that one case `keep` acts like
  `repair`. If the head already had one, `keep` removes only exact duplicates.
  A late `rel="canonical foo"` loses only the canonical token.
- `hreflang`: under `repair` and `replace` a late link is removed. Under `keep`
  it is removed only when the pack inserted the cluster and the link's language
  code is in it.
- In report mode, or when the head plan was dropped, nothing is removed and the
  decision is logged.

## The Link header action

A canonical rule can carry `"link_header": "match"` (default `"off"`). The filter
then keeps the `Link: <url>; rel="canonical"` response header consistent with the
canonical the page ends up with, at the end of the head:

| Origin `Link` header | `keep` | `repair` / `replace` |
|---|---|---|
| No canonical entry | Adds `<url>; rel="canonical"`, only if the page's final canonical equals the rule's own value | Same |
| Entries agree with the page | Nothing | Nothing |
| An entry differs | Nothing; logs `header_conflict` | Removes the conflicting canonical entries, keeps every other entry (preload, preconnect, ...), adds the page's canonical if none is left |

A `keep` page that keeps its own canonical gets no header. A page whose head plan
was dropped makes no header decision. Report mode never writes the header. A
`Link` line the filter cannot parse completely is left as it is.

The action sees the headers the module sees when it runs. A `Link` header added
later, for example by Apache `Header always set`, or by another module that runs
after the PageSpeed output filter, is invisible to it and is not de-duplicated.
Check the `Link` headers on the wire before you enable the action. The action is
skipped once the first part of the response has been sent (`headers_flushed`).
On an Apache host whose pack has a canonical rule with `link_header` set to `match`,
response headers go out with the first body bytes or at the end of the response, not
on an early flush, so an origin that flushes its headers early to start browser
preloads loses that head start. A pack without such a rule does not change when
headers go out. The first response in each server process can still be held until
the pack has been read.

### HEAD requests on Apache

A pack edits the `Link` header while it parses the HTML body, and a `HEAD`
response has no body. So on Apache a `HEAD` request returns the origin's `Link`
header, and the `GET` of the same URL returns the edited one. A crawler that
probes with `HEAD` can see a different canonical than a browser does. To see
what the pack serves, use `GET`.

The other `Link` entries (preload, preconnect, ...) are the origin's on both.
The module removes the origin's `Content-Length` from HTML it rewrites, so a
`HEAD` response to such a page carries no `Content-Length` header (not `0`),
while the `GET` response can carry the length of the body it sends. Do not rely
on a `HEAD` response for the size of a page.

## Daemon servers

On Apache, nginx and IIS the optimizer daemon serves no HTML, and the module
records none into it. Every HTML response is rewritten by the module's own
filter chain, which carries the pack, whether or not `rule_packs` is enabled.
CSS, JavaScript and images are served and recorded by the daemon as before.
`ipro_daemon_fallthrough` rises on HTML requests as part of this normal
daemon behaviour.

## Decision log

Where: `<LogDir>/packs/<pack-id>/decisions-YYYY-MM-DD.jsonl`, with a UTC date.
Each pack on a server has its own directory, named by its id, and a rule's
lines go to the log of the pack it belongs to, so a `claimed` line is in the
log of the later pack. `LogDir` is the existing option and
must be writable by the server user. On Linux and macOS the files are created
with mode 0640 (readers need the server's group); on Windows they inherit the
directory's ACL. With an empty `LogDir` there is no log (the
counters still work) and one warning is written per process.

One JSON object per line, one line per decision. Fields: `ts` (milliseconds
since the epoch), `pack` (`id@version`), `sha256` (first 16 hex digits of the
pack file's hash), `host`, `path` (with the query, cut at 300 bytes), `rule`,
`kind`, `mode`, `action` (`insert`, `fill`, `replace`, `remove`, `dedupe`,
`none`), `reason` (such as `equal`, `present`, `late_conflict`,
`not_rewritable`, `claimed`), `claimed_by` (only on a `claimed` line: the id of
the pack that owns the kind), `defect` (always present, empty when none),
`before` (a hash), `after` (cut at 200 bytes), and `removed` when elements were
removed. The line for the `Link` header action carries `"header": true`. Page
values are hashed, except `removed`, which holds page values (hrefs, `@type`s)
in clear text.

When a host runs more than one pack, the packs load in configuration order and
the first pack whose rule is selected for a page (its site lists the host and
the rule's path scope matches) owns that rule's kind on the page; for a
`link_header: match` rule it also owns the `Link` header. A later pack's rule
of the same kind acts on nothing and logs action `none`, reason `claimed`, and
`claimed_by` with the id of the owning pack. Ownership does not depend on the
modes: a first pack that only reports still keeps a later enforcing pack off
the kind, and it keeps it even when its own change is dropped as
`not_rewritable`. Two rules of one kind in the same pack never log `claimed`
(the first matching rule wins, as with one pack), so a host with one pack never
sees the reason or the `claimed_by` field. When two packs on a host carry rules
of the same kind and their modes differ, in either order, one warning per process
says which pack owns it: `rule pack overlap: <kind> owned by <first id>
(report) before <later id> (enforce); the later pack will not act on it`.

The log does not record the client address, cookies or user agent. The path is
logged with its query string, so treat the log as you would an access log.

One rule can log more than one line for a page: the head decision, the late
element decision, and the header decision.

Repeats: a process writes a line at most once per key. The key is host, path,
rule, pack hash, mode, action, reason and the header marker. The processes of a
server do not coordinate, so N processes can each write the same line, and a
process clears its key set when it reaches 65,536 keys and can then repeat a
line. Count distinct keys, not lines.

Size and age: all processes together write about 50 MiB per day. The size is
checked every 256 lines, so it can overshoot. After that, lines for the rest of
that day are dropped. `rule_pack_decision_log_dropped` counts those, and also
failed writes and a directory that cannot be created. The newest 14 calendar
days are kept (today and 13 earlier); the process that creates a day's file
deletes older ones. A process only writes the current day's file and reopens when
the date changes. Do not delete or rotate the current day's file: a process
that still holds a deleted file keeps writing into it and those lines are lost.

## Statistics

On the statistics page; per virtual host with `UsePerVHostStatistics on`.

- Load: `rule_pack_loads`, `rule_pack_load_errors`
- Signatures: `rule_pack_loads_signed`, `rule_pack_refused_unsigned`,
  `..._signature_unreadable`, `..._publisher_mismatch`, `..._unknown_key`,
  `..._bad_signature` (see [Signed packs](#signed-packs))
- Pages: `rule_pack_pages_evaluated`, `rule_pack_pages_modified`,
  `rule_pack_pages_would_modify`
- Pages skipped: `rule_pack_pages_skipped_host_not_listed`, `..._no_head`,
  `..._malformed_head`, `..._not_rewritable`, `..._size_limit`
- Decisions: `rule_pack_decisions_applied`, `rule_pack_decisions_would_apply`,
  `rule_pack_decisions_claimed` (a rule kept off a page by an earlier pack)
- Duplicate pack ids: `rule_pack_refused_duplicate_id` (a file whose pack has
  the id of a pack loaded earlier on the host; once per process and file
  version)
- Late elements: `rule_pack_late_removed`, `rule_pack_late_unremovable`
- Header (move only with the Link header action): `rule_pack_headers_set`,
  `rule_pack_headers_flushed`
- Log: `rule_pack_decision_log_lines`, `rule_pack_decision_log_dropped`
- `rule_pack_pages_skipped_error` is reserved and always 0.

There are no per-rule counters. Per-rule numbers come from the decision log.
With several packs on a host the page counters move once for each pack that
evaluated the page (see [Page counters with two packs](#page-counters-with-two-packs));
per-pack numbers come from the decision log too.

## Limits

A pack that exceeds a load limit does not load.

| Load limit | Value |
|---|---|
| Pack file size | 1 MiB |
| Rules | 200 |
| Table entries (all tables) | 50,000 |
| Template size | 16 KiB |
| Distinct path patterns | 2,000 (32 per list in a rule) |
| Path pattern | 512 bytes |
| Regular expression | 1,024 bytes, 1 MiB of memory |
| Rule id | 64 characters |
| Host | 253 characters |
| JSON nesting in the pack | 32 levels |
| JSON-LD template nesting | 64 levels |

Templates are checked when the page is built; literal table values are checked
at load, so the same limit can be a load error. At page time, a value over
these limits is not applied and the decision is `invalid_value`:

| Page-time limit | Value |
|---|---|
| URL | 2,048 bytes |
| Title | 300 characters |
| Description | 1,000 characters |
| Expanded JSON-LD | 16 KiB |

The added-bytes budget is 64 KiB per page, shared by the head and document-end
plans. It counts 256 bytes per change more when `debug` is on. If JSON-LD
overflows it, only the JSON-LD plan is dropped (`size_limit`) and head changes
stay. A JSON-LD block over 1 MiB, or nested deeper than 64 levels, is not parsed, and
that rule then leaves the page's JSON-LD alone. A template nested deeper than 64
levels is a load error.

## Signed packs

A pack can carry a signature, so the server can tell a pack from a publisher
you trust from a file that was edited or copied. Signing is optional and off by
default: without any of the settings below, every pack that loads is used, as
before.

**The signature.** The signature is an Ed25519 signature over the exact bytes of
the pack file. It lives in a sidecar next to the pack, named after it with
`.sig` added (`example.com.json.sig`). If the pack path is a symlink, the sidecar
sits next to the file the link points to. The sidecar is one line, at most 1,024
bytes:

```
ed25519 <key id> <base64 signature>
```

The key id is lowercase letters, digits and `-` (1 to 64 characters) and must
equal the pack's `publisher`. The signature is 88 base64 characters. Anything
else in the file makes the signature unreadable.

**Trusting a key.** `RulePackTrustKey <key id> <base64 public key>` lists a key
the server trusts. The key is 44 base64 characters (32 bytes). You can repeat
the directive, up to 16 keys per server or virtual host. A virtual host adds to
the server's keys, and a virtual host key with the same id replaces the server's.
Packs signed by the We-Amp publisher key (key id `we-amp`) verify without a
`RulePackTrustKey` line; the directive adds your own keys, and a key you list
with the id `we-amp` replaces the built-in one.
`RulePackRequireSigned on|off` (default `off`) decides what happens to a pack
that fails the check. Both directives are server-scoped, like `RulePackFile`: they cannot be
set in `.htaccess`, `<Directory>`, an nginx `location` block or an IIS match
block, so a per-directory configuration can neither add a key nor relax the
requirement. On Apache the names carry the usual `ModPagespeed` prefix.

```apache
ModPagespeedRulePackTrustKey example-publisher AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=
ModPagespeedRulePackRequireSigned on
```

```nginx
pagespeed RulePackTrustKey example-publisher AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=;
pagespeed RulePackRequireSigned on;
```

On IIS, put `RulePackTrustKey example-publisher <key>` and `RulePackRequireSigned on`
at the top level of the site's `pagespeed.config`. The key above is an example,
not a real key. `RulePackTrustKey` takes two values and is not available on Envoy.

**How a pack is checked.** The server runs these checks in order. The first one
that fails names the reason.

1. `unsigned`: there is no `.sig` file.
2. `signature unreadable`: the sidecar cannot be read or is not in the format above.
3. `publisher mismatch`: the key id in the sidecar is not the pack's `publisher`.
4. `unknown key`: that key id is neither the built-in `we-amp` key nor listed by a `RulePackTrustKey`.
5. `bad signature`: the signature does not match the pack's bytes and that key.

A pack that passes all five is `verified`. The check runs when the file is
loaded and again when the sidecar changes. The pack file's SHA-256 stays the
pack's identity in the log.

**What you see, with `RulePackRequireSigned off` (the default).** Every pack
that loads is used. A verified pack logs once per file version:

```
rule pack loaded signed: <id>@<version> key=<key id> sha256=<hash> path=<path>
```

Any other pack logs the existing warning `rule pack loaded unsigned: ...`. When a
`.sig` file exists but fails one of the checks 2 to 5, a second warning says why:

```
rule pack signature not verified (<reason>): <id>@<version> sha256=<hash> path=<path>
```

**What you see, with `RulePackRequireSigned on`.** A pack that fails any check is
refused and not used. If an earlier version of the same file was accepted with the
same trust settings, it stays in use; otherwise the filter is off for that
server or virtual host. One warning is logged for each refused version:

```
rule pack refused (<reason>): <id>@<version> sha256=<hash> path=<path>; keeping the pack loaded earlier
rule pack refused (<reason>): <id>@<version> sha256=<hash> path=<path>; the rule_packs filter is off for it
```

The matching counter goes up once, per process and file version:
`rule_pack_refused_unsigned`, `rule_pack_refused_signature_unreadable`,
`rule_pack_refused_publisher_mismatch`, `rule_pack_refused_unknown_key` or
`rule_pack_refused_bad_signature`. A verified pack counts
`rule_pack_loads_signed`. `rule_pack_loads` still counts a file that parsed and was
then refused. The Rule packs page of the admin site does not show signature
results, and a refused pack still shows as loaded there; use the log and the
statistics.

**The signing tool.** `tools/rule-packs/sign-pack.sh` needs bash and OpenSSL 3.0
or newer (set `OPENSSL=/path/to/openssl` to choose the binary; LibreSSL does
not work). It exits with status 2 and says so when a tool is missing. The tool
ships in the source tree, not in the packages.

```
sign-pack.sh keygen <key id> <private-key.pem>
sign-pack.sh pubkey <key id> <private-key.pem>
sign-pack.sh sign   <key id> <private-key.pem> <pack.json>
sign-pack.sh verify <pack.json> <base64 public key>
sign-pack.sh --self-test
```

- `keygen` creates a new private key file (it refuses to overwrite one) and
  prints the `RulePackTrustKey <key id> <public key>` line to put in the server
  configuration. `pubkey` prints that line again for an existing key.
- `sign` writes `<pack.json>.sig`, and warns when the pack's `publisher` is not the
  key id you signed with (the server would report a publisher mismatch).
- `verify` checks a pack and its sidecar against a public key and prints
  `verified: key=<key id>`, `bad signature` or `signature unreadable`.
- `--self-test` signs a sample pack with throwaway keys, verifies it, flips one
  byte and expects a refusal, then prints five `PASS:` lines. It touches nothing
  outside a temporary directory.

Keep the private key off the servers and out of any repository. Sign after the
last edit: any change to the pack, even a line ending, needs a new signature.

## Admin page

`/pagespeed_admin/rule_packs` is a read-only Rule packs page in the admin
pages. It is served by the same admin handler as the other admin pages, so the
access restrictions you keep for them apply to it: on Apache the
`AdminDomains` directive, and in production a firewall or ACL; on nginx the
`location` rules for `/pagespeed_admin/` from the install guide; on IIS the
site bindings or IP restrictions. It answers `GET` and `HEAD` only (other
methods get 405) and is never cached. The admin console's sidebar links to the
page.

For the virtual host the request reached, it shows:

- whether the `rule_packs` filter is enabled, `RulePackFile` and `RulePackMode`;
- every `RulePackFile` of the host, in load order, and for each the pack the
  server process that answers has loaded: id, version, publisher, SHA-256,
  load state (loaded, failed and keeping the earlier pack, refused, none, or
  duplicate id for a file whose pack id an earlier file already uses), the
  time of the last load event, the last load error and the loader's warnings;
  the overlap warnings between packs. Looking at the page never loads a pack:
  a process that has not yet served a page that needs the pack shows none, and
  another process can show a different state;
- the `rule_pack_*` statistics;
- two static links, to the packs catalogue and to this documentation; the server makes
  no request for them;
- the newest lines of today's decision log, newest first. Today is the UTC
  date of the day file. The log is shared by all server processes, so these
  lines do not depend on which process answers. A per-host page shows only the
  lines of that host; the whole-server admin page shows every host.

Query parameters: `?rule=`, `?action=` and `?reason=` keep only matching
lines; `?pack=<id>` selects the pack whose log is shown (default: the pack of the first
`RulePackFile`, if this process has loaded it; otherwise the logs of the pack
directories under `<LogDir>/packs/` are merged, looking at no more than 256
directory entries and 8 pack ids); `?limit=` caps the number of lines shown (default 100, at most 1000);
`?raw=1` returns the selected lines as plain text, one JSON object per line
(for a host with one pack, the same text as before).
The page reads at most the last 1 MiB of the day file, so on a busy day the
oldest lines of the day are not shown. With an empty `LogDir` the page says the
log is off.

### JSON status

`/pagespeed_admin/rule_packs?json=1` returns the same status as one JSON
document (`application/json`), for scripts and monitoring. It takes the same
`?pack=`, `?rule=`, `?action=`, `?reason=` and `?limit=` parameters. The top
level keys are:

| Key | Content |
|---|---|
| `schema` | `1`. It changes only when a key is removed or changes meaning; new keys can appear without a change. |
| `scope`, `host`, `generated_at` | The scope the page describes, its host, an ISO-8601 UTC time. |
| `configuration` | `filter_enabled`, `rule_pack_files` (in load order), `rule_pack_mode`, `rule_pack_require_signed`. |
| `packs` | One object per file, in load order: `position`, `file`, `state`, `id`, `version`, `publisher`, `key_id`, `sha256`, `last_event_at`, `last_error`, `warnings`, `license` (the pack's `license` object as written in the file, or `null`). `state` is one of `loaded`, `failed-keeping-earlier`, `refused`, `none` or `duplicate-id`. |
| `overlap_warnings` | The overlap warnings, as text. |
| `counters` | The `rule_pack_*` statistics, or `null` when statistics are off. |
| `decisions` | `date`, `pack`, `limit`, `status`, `error` and `rows`. `status` is `ok`, `log_dir_not_set`, `no_pack_file`, `error` or `missing`; `error` is `null` unless `status` is `error`. `rows` are the selected log lines, newest first for one pack. In the merged listing (see `?pack=` above) the per-pack tails are joined in id order and reversed, so rows are not in time order across packs. A row has `at`, `ts_ms`, `pack_id`, `pack_version`, `sha256_prefix`, `host`, `path`, `rule`, `kind`, `mode`, `action`, `reason`, `defect`, `before`, `after`, `claimed_by` (`null` unless the reason is `claimed`), `removed` and `header`. |

Keys are snake_case. A pack that this process has not loaded yet has `null`
for `id`, `version`, `publisher` and `sha256`, as on the HTML page.

The admin console has a Rule packs view with the same status and a shorter
decision table.

## Operating it

- Turn it off: remove the directive and reload. Deleting the pack file does
  not turn it off; the process keeps the loaded pack and logs `cannot be read
  ... keeping the pack loaded earlier`.
- After a pack change, processes can disagree for up to 10 seconds. A new child
  that finds a bad file has no pack, while old children keep the good one.
- Do not delete or rotate the current day's decision log. Check who can read it
  (0640 on Linux and macOS).
- `ipro_daemon_fallthrough` rises on HTML requests on daemon servers by design.
- To learn why a path was not touched, request it with
  `?PageSpeedFilters=+debug` and check the host, the path patterns and the three
  mode settings.

## Troubleshooting

- Nothing happens. Check, in order: (1) the filter is enabled and
  `RulePackFile` is set (otherwise the debug output says "no rule pack file");
  (2) the pack loaded ("no usable rule pack"); (3) the host is listed in `sites`
  (`rule_pack_pages_skipped_host_not_listed`); (4) a rule's paths match and are
  not excluded (no counter; use `?PageSpeedFilters=+debug`); (5) the page has a
  well-formed head, which is only evaluated once a rule matched (`no_head`,
  `malformed_head`); (6) the mode is not still report at one of the three
  levels.
- A second pack does nothing. Check the log of the later pack for `claimed`
  lines (an earlier pack owns the kind), the error log for `rule pack refused
  (duplicate id)`, and the overlap warning in the admin page.
- The pack did not load. `rule_pack_load_errors` goes up and the error log has a
  warning, `rule pack <path> (sha256=<64 hex>) failed to load: <path>: <json
  path>: <reason>; keeping the pack loaded earlier` (or `; the rule_packs filter
  is off for it` when there was no earlier pack). An unreadable file logs `rule
  pack <path> cannot be read: <os error>; ...`. A successful load of a pack
  without a valid signature logs the warning `rule pack loaded unsigned:
  <id>@<version> sha256=... path=...` once per file version per process; a
  verified pack logs `rule pack loaded signed: ...` instead.
- `not_rewritable`. A flush split the head and the part the rule has to change
  was already sent. The filter then drops all head changes for that page and
  leaves the head as the origin sent it. Inserts into a head that is still open
  still work.
- `headers_flushed`. The first part of the response left before the head ended,
  so the `Link` header could no longer be changed. Other changes are not
  affected.
- `skipped_malformed_head`. An inert element (for example `svg`) was left
  unclosed in the head and swallowed the rest of it, so the page was skipped.
- The pack only sees responses the module rewrites: status 200, HTML, not
  already marked `X-Mod-Pagespeed`. If a CDN or browser caches your HTML, a pack
  change reaches visitors only as those copies expire.
