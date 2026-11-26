- The admin console's HTML cache panel now covers `DaemonHtmlCache
  serve-variants`: the mode, entries stored and removed, Markdown answers,
  why requests fell through to the page, and the reasons the policy refuses a
  store. It names the states in which serving is not active (optimizer
  library below 1.14, optimizer not in a markdown mode, agent-optimize off)
  and shows the counters that should stay at zero as warnings. Every new
  counter is described on the statistics page.
