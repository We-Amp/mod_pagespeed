- **IIS: the module can now reach an optimizer worker that runs under
  another account.** The module's check for the worker's pipe asked for
  more access than the worker grants other accounts, so with the module in
  an application pool identity and the worker under a service account every
  daemon-backed request reported `X-Pagespeed-Init-Status:
  daemon-unavailable`. The check now asks only for read and write access.
