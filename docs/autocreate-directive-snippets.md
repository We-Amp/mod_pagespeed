# AutoCreateCachePath / AutoCreateLogDir — directive reference snippets

The customer-facing 1.1 directive reference lives in the
`pagespeed-optimizer` repo under `website/` (the source of `modpagespeed.com`),
not in this repo. These snippets are drop-in entries for the directive
reference page there. Add them under the IIS-scoped section, ordered
alphabetically alongside the other boolean directives (e.g.
`EnableCachePurge`, `Statistics`). Match the surrounding formatting
exactly — do not re-style.

---

### `AutoCreateCachePath`

| | |
|---|---|
| **Values** | `on` \| `off` |
| **Default** | `on` |
| **Scope** | IIS only |

When `on` (the default), the IIS worker auto-creates the per-site
cache subdirectory under the configured `FileCachePath` at startup,
applying ACLs only when the inherited grant from the installer is
insufficient. When `off`, the worker requires the per-site
subdirectory to exist before startup and falls through to the
diagnostic error page on absence.

Limited to paths under `C:\ProgramData\We-Amp\PageSpeed\cache\` or
`C:\ProgramData\We-Amp\IISWebSpeed\cache\` — out-of-prefix paths
always require manual creation regardless of this setting.

Apache and nginx create the cache directory at directive-parse time
with no opt-out; this directive has no effect on those ports.

---

### `AutoCreateLogDir`

| | |
|---|---|
| **Values** | `on` \| `off` |
| **Default** | `on` |
| **Scope** | IIS only |

When `on` (the default), the IIS worker auto-creates the configured
`LogDir` at startup if it does not exist, applying ACLs only when the
inherited grant from the installer is insufficient. The runtime grant
is RX+W (read, execute, write — no DELETE), mirroring the installer's
narrower `GrantLogAcl`: workers append to logs but admin owns
rotation. When `off`, the directory is left untouched at startup and
log writes fail silently on absence (pre-2026 behaviour).

Limited to paths under `C:\ProgramData\We-Amp\PageSpeed\logs\` or
`C:\ProgramData\We-Amp\IISWebSpeed\logs\` — out-of-prefix paths
always require manual creation regardless of this setting.

Apache and nginx create the log directory through their own
directive-handler logic with no opt-out; this directive has no
effect on those ports.

---

## Notes for the orchestrator

- File the actual edit against `pagespeed-optimizer/website/` in the
  directive reference page (search the website source for
  `EnableCachePurge` to locate it).
- Keep the entries to the decided scope: no marketing copy, no
  "auto-heal" framing, no MSI-property reference.
- As decided for these directives, the module also emits a single INFO log
  line at startup naming the resolved state
  (`AutoCreateCachePath: on`). This is operator-facing log output,
  not directive-reference content, and does not belong in the entry
  above.
