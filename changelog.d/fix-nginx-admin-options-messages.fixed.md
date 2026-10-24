- **nginx: an admin POST refused for its body no longer strands the request's
  fetch.** An oversized or unreadable body on an admin POST was answered
  413, but the fetch object created for the request was never completed: it
  leaked a little memory per request and held every later graceful stop or
  reload of that worker for the full 30-second shutdown budget. The fetch is
  now completed on the refusal path.
- **nginx: a request whose scripted option value is invalid is served without
  optimization instead of dropped.** With `ProcessScriptVariables` on and a
  `pagespeed` directive whose value comes from an nginx variable, a value the
  configuration rejects for that request (for example one derived from a
  `$arg_*` or `$http_*` variable) closed the connection and leaked the
  request's option copies. The request is now served unoptimized.
- **nginx: the message-history endpoint reports its refusals.** A non-GET
  request to the grouped message history, or a second one while a grouped
  read was in flight, answered 200 with the error JSON. The endpoint now
  sends the status the shared handler produced (405 with
  `Allow: GET, HEAD`, or 429 when busy), so monitoring and scripts see the
  failure.
