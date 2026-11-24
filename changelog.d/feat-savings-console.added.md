- **The admin console shows delivered savings.** The Savings page opens with a
  new card, Delivered savings: per type (images, CSS, JavaScript) the size of
  the originals, the size sent and how much smaller that was, counted each time
  an optimized response is sent, from the cache as well. Responses the server
  optimized itself and responses answered from the optimizer cache are shown
  side by side and never added together; the second can be in sizes after
  compression, and the card says so. A type sent larger than its originals shows
  as a negative saving. Before, the page showed only what was saved at the
  moment a resource was optimized, which stands still on a warm cache and read
  as little or no savings while the site served optimized resources all day.
  That figure is still there, now titled "Saved at rewrite time" and described
  for what it counts.
  After a restart or reload the counters start again from zero: the card then
  says that nothing was sent yet instead of showing 0%, and holds back
  percentages for the first 15 minutes. With statistics logging on
  (`StatisticsLogging on` and a `LogDir`), it also shows the same figures over
  the history the statistics log holds, restarts included; with logging off it
  says which settings turn it on. The Overview's first tile and its status
  sentence state what the server's own rewriting saved in delivery once an
  optimized response was sent, and the Savings chart draws that as a third
  line, which goes below zero while responses go out larger than their
  originals.
