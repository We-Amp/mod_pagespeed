- **An ExperimentSpec in the main configuration that sets options above directory scope now fails at load.** Set such options outside the ExperimentSpec.
- **Apache: .htaccess files with MapOriginDomain, MapProxyDomain or CustomFetchHeader now give a 500 error.** Move these directives to the server or virtual-host configuration.
- **An ExperimentSpec that sets an alternate origin domain is now refused in
  .htaccess files (500 error) and in remote configuration.** Set it in the
  server or virtual-host configuration.
