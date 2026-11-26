- **Validators the module issues itself never reach the origin.** When a
  request's `If-None-Match` names an entity tag reserved for the module (one
  whose opaque tag starts with `psh-` or `psm-`, weak or strong), the module
  now removes `If-None-Match` and `If-Modified-Since` from the request before
  any handler evaluates them or any proxy forwards them, on Apache, nginx, IIS
  and Envoy, whatever the configuration. Every other validator is forwarded
  unchanged.
- **`Vary: Accept` on every HTML response in serve-variants mode.** With
  AgentOptimize on and `DaemonHtmlCache` at `serve-variants`, every rewritten
  HTML response now carries `Vary: Accept`, not only one to a request that
  asks for Markdown.
