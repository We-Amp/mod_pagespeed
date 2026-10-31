- **Responses to requests that carry an `Authorization` header no longer get
  the short shared-cache lifetime used for in-place optimization.** The
  configured in-place lifetime is unchanged for other responses.
