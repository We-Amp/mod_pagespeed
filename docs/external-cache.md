# External cache (Redis and memcached)

The server-side cache can live in Redis or memcached instead of (in addition
to) the file cache. Directives are shown in Apache form
(`ModPagespeedRedisServer`); nginx uses `pagespeed RedisServer ...;` and IIS
uses the same names in its configuration. The Envoy filter takes its Redis
settings from its own configuration, not from these directives.

## Servers

| Directive | Value |
|-----------|-------|
| `RedisServer` | `host[:port]` (port defaults to 6379) or `unix:/absolute/path`. |
| `MemcachedServers` | Comma-separated `host[:port]` and `unix:/absolute/path` entries. |

A `unix:` entry names a Unix domain socket and takes no port. Unix sockets are
not available on Windows. Socket paths cannot contain commas.

## Redis authentication

| Directive | Default | Meaning |
|-----------|---------|---------|
| `RedisPassword` | unset: no `AUTH` is sent | Password sent with `AUTH` right after every connection the module opens, including reconnects and every node of a cluster. |
| `RedisUser` | unset: the default user | Redis ACL user for `AUTH`. Ignored without `RedisPassword`. |

```
ModPagespeedRedisServer "unix:/var/run/redis/redis.sock"
ModPagespeedRedisPassword "password"
ModPagespeedRedisUser "pagespeed"
```

- The password is never written to the logs; configuration pages, the JSON
  output and the admin console show it as `***`.
- If the server has no password configured, `AUTH` is refused and the
  connection is not used. A rejected `AUTH` is logged without the password and
  retried after `RedisReconnectionDelayMs`.
- `AUTH` does not encrypt traffic: prefer a Unix socket or a private network,
  and keep the configuration file readable only by the account that runs the
  server.
- An ACL user needs only `GET SET SETEX DEL SELECT INFO CLUSTER ASKING`:

```
ACL SETUSER pagespeed on >password ~* -@all +get +set +setex +del +select +info +cluster +asking
```
