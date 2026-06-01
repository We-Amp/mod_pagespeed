# pagespeed/kernel/

Foundation library shared by all deployment modes (Apache, Nginx, Envoy, IIS).
Contains base utilities, data structures, HTML/HTTP handling, image processing,
caching, threading, and shared-memory primitives. No server-specific code lives
here -- everything is portable.

## Subdirectory Index

| Directory | Purpose |
|-----------|---------|
| `base/` | Core types: `GoogleString`, `StringPiece`, `MessageHandler`, `Statistics`, `Timer`, `FileSystem`, `Writer`, `Hasher` (MD5), `AbstractSharedMem`, `CacheInterface` |
| `cache/` | Cache implementations: `LRUCache`, `CycloneCache`, `CacheBatcher`, `AsyncCache`, `CacheStats`, `PurgeContext` |
| `html/` | Streaming HTML parser/lexer, `HtmlParse`, `HtmlFilter`, `HtmlElement`, `HtmlNode`, `AmpDocumentFilter` |
| `http/` | HTTP primitives: `ContentType`, `GoogleUrl`, `RequestHeaders`, `ResponseHeaders`, `CachingHeaders`, `BotChecker` |
| `image/` | Image codec wrappers: GIF, PNG, JPEG, WebP readers/optimizers; scanline-based processing pipeline |
| `js/` | JavaScript tokenizer, minifier (`JsMinify`), keyword tables |
| `license/` | Ed25519-based license token generation and validation |
| `license_v2/` | V2 license file format and verification |
| `sharedmem/` | Shared-memory abstractions: `InProcessSharedMem`, `SharedCircularBuffer`, `SharedMemStatistics`, `SharedMemLockManager` |
| `thread/` | Threading primitives: `PthreadSharedMem`, `SchedulerThread`, `EventScheduler`, `EventDispatcher`, `QueuedWorkerPool` |
| `util/` | Platform helpers, `NonceGenerator`, `CopyOnWrite`, `BrotliInflater`, `Gzip`, `ReEncoder` |

## Key Abstractions

- **RewriteDriver** (in `net/instaweb/`) -- per-request optimization coordinator; kernel provides its dependencies
- **RewriteOptions** (in `net/instaweb/`) -- configuration object; kernel types like `Timer`, `Hasher`, `Statistics` are injected
- **MessageHandler** -- abstract logging interface; each deployment mode provides its own implementation
- **CacheInterface** -- `Get`/`Put`/`Delete` with async callbacks; composed via `CacheBatcher`, `CacheStats`, `AsyncCache`
- **HtmlParse** -- streaming SAX-style HTML parser that drives a chain of `HtmlFilter` instances
- **AbstractSharedMem** -- factory for named shared-memory segments with mutex support

## Namespace

All kernel code lives in `net_instaweb::` (legacy). JS minification uses
`pagespeed::`. Do not introduce new uses of the `net_instaweb` namespace
in new code; it is retained for backward compatibility.

## Shared Memory Implementations

| Implementation | Used By | Notes |
|----------------|---------|-------|
| `PthreadSharedMem` | Apache, Nginx, Envoy (Linux) | mmap + `pthread_mutexattr_setpshared`; segments must be created before fork |
| `InProcessSharedMem` | IIS (Windows) | Per-process only; no cross-process sharing |
| `NullSharedMem` | Fallback | No-op; used when shared memory is unavailable |

## Testing

```bash
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/...
```

Tests mirror the source layout: `test/pagespeed/kernel/base/` tests
`pagespeed/kernel/base/`, and so on. Image codec tests require the
image libraries (libjpeg-turbo, libpng, libwebp, giflib) to be linked.
