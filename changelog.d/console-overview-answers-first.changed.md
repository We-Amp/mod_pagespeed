- **The admin console Overview answers first.** It opens with one sentence that
  says whether the console is working, with the main figure and the time it
  covers, followed by a row of headline tiles: savings from rewriting, savings
  by the optimizer's cache, resource fetch failures and the optimizer's state.
  When a finding needs action, the findings count carries a Warning or Error
  badge and links to the findings. A fresh install lists next steps instead of
  empty figures.
- **Each cause is listed once.** A log warning that already has its own finding
  is no longer repeated under "Warnings in the log", and quoted log text is
  folded under the written cause.
- **Savings reads at a glance.** The split bars show three named shares with a
  legend, the per-host breakdown has its own card, the columns stay balanced,
  and while the optimizer is still warming up the page says it is too early
  for a percentage instead of showing a dash.
- **One vocabulary across the admin console.** Each figure has one name on
  every page ("Saved by rewriting", "Saved by the optimizer's cache", "Served
  optimized"), the host control is called "Host filter", Logs moved to the top
  of the menu, health checks have plain names, and an optimizer that does not
  answer is described the same way everywhere. The About page maps the old
  console's page names to the new ones.
- **Admin console pages share one look.** Tables, cards, badges, notes and
  headings are styled the same on every page, and body text is a steady
  14 px.
- **The admin console top bar shows connection and scope.** A pill reads
  "Connected" or "Reconnecting…", a chip names the scope (whole server or the
  host), and with no optimizer configured its menu group is muted and marked
  "not configured".
- **The auto-refresh control says whether it is on or off**, a pause is
  remembered across reloads, and a refresh you ask for reports whether it
  worked: "Refreshed", "Partly refreshed", "Server busy" or "Refresh failed".
