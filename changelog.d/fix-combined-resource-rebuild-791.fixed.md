- **Combined stylesheets and scripts no longer answer 404 when an input is
  briefly unavailable.** Before, once a combined CSS or JavaScript file was
  no longer cached and one of its inputs could not be fetched at that moment,
  the combined URL answered 404 and the page lost all of its styling or
  scripts. The combined URL is now served with the inputs that can be fetched,
  with a short private lifetime, and is complete again once the missing input
  is back.
