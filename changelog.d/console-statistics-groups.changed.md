- **The admin console Statistics page groups counters by what they measure.**
  Before, counters were grouped by the first word of their name, which gave
  dozens of small groups such as "Not", "In", "Show" and "Num". Now there are
  about twenty named groups, most useful first: page rewriting, in-place
  optimization, the optimizer's serving and HTML cache, rule packs, each cache
  backend, origin fetches, images, CSS, JavaScript, purges and the server
  process. Counters a newer module adds before the console knows them are
  listed last, under "Other". Screen readers announce each group's size as a
  number of counters.
- **Every counter on the Statistics page has its own description**, including
  the rule pack counters, the per-page rewrite limits, each reason the HTML
  cache report gives for not storing a page, and the CSS parse size limit.
