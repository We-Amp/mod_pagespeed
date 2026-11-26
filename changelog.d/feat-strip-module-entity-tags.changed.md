- **Validators the module issues itself never reach the origin.** When a
  request's `If-None-Match` names an entity tag the module issues for its own
  variants (`psh-` and `psm-` tags, weak or strong), the module now removes
  `If-None-Match` and `If-Modified-Since` from the request before any handler
  evaluates them or any proxy forwards them, on Apache, nginx, IIS and Envoy,
  whatever the configuration. Every other validator is forwarded unchanged.
  Such a request is answered with a full response instead of a `304` the
  origin could not have meant.
