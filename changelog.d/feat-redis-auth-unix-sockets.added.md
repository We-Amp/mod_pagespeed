- **Redis authentication, and Unix sockets for Redis and memcached.** The new
  `RedisPassword` option (with an optional `RedisUser` for Redis ACL users)
  makes the Apache and nginx modules authenticate after every connect and
  reconnect, so a Redis server that requires a password can be used as the
  external cache. The password is never logged and shows as `***` in
  configuration output. `RedisServer` and `MemcachedServers` also accept
  `unix:/path/to/socket` (not available on Windows).
