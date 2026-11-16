- **IIS: do not send the RequestOptionOverride token in a response header.**
  IIS does not read it from responses, and web.config customHeaders send it
  to every client. If it was ever set there, choose a new token.
