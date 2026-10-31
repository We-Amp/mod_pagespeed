- **HTML proxy path: the Vary tokens for rewritten HTML are added before the
  response is handed to the rewriter thread.** A page that is passed through
  unparsed because it exceeded the learned HTML parse-size limit now carries
  the same Vary tokens as its rewritten form, and the response headers are no
  longer updated by one thread while the fetch thread is still reading them.
