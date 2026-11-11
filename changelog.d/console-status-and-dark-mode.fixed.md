- **The admin console no longer looks healthy while the server is down.** When
  the module stops answering, Overview says "Status unknown" with the time of
  the last answer instead of a green verdict, and the optimizer's state is
  marked "last known". The "updated N s ago" stamp now counts up on every page
  and says "refresh overdue" once three refreshes have been missed.
- **A page that fails to load explains why.** Instead of a raw message such as
  "HTTP 404: Unknown admin page", the console names what could not be read, the
  likely cause (no answer, not available on this server, sign-in or address
  restriction, server or proxy error), a docs link and a "Try again" button.
  The original message stays available under "Technical details".
- **Optimizer status and Overview agree.** Without an optimizer configured the
  page shows one explanation with a docs link instead of "Optimizer unreachable"
  above empty sections, and an optimizer that is running but did not answer one
  refresh is no longer reported as stopped.
- **The Savings page no longer requests the message log** it never displayed.
