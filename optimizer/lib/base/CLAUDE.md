# Base Library

Foundational utilities shared across all components.

## Key Files
- `string_util.h` -- string helpers not in `std::` or `absl::` (keep minimal)
- `message_handler.h` -- `MessageHandler` abstract base, `NullMessageHandler`, `ConsoleMessageHandler`
- `json_message_handler.h` -- `JsonMessageHandler`: captures messages as JSON (for worker API)
- `tee_message_handler.h` -- `TeeMessageHandler`: forwards to multiple handlers
- `writer.h` -- `Writer` abstract base (`net_instaweb::` namespace)
- `string_writer.h` -- `StringWriter`: writes to `std::string`
- `null_writer.h` -- `NullWriter`: discards output
- `url_util.h` -- URL parsing/normalization helpers
- `symbol_table.h` -- `SymbolTable`: interned string storage (used by HTML parser)
- `arena.h` -- arena allocator for HTML parser nodes
- `basictypes.h` -- `DISALLOW_COPY_AND_ASSIGN` macro (legacy)
- `printf_format.h` -- printf format attribute macros

## Namespaces
- `pagespeed::` -- `MessageHandler`, `MessageType`, `LogLevel`, string utilities
- `net_instaweb::` -- `Writer`, `StringWriter`, `NullWriter` (legacy)

## Testing
```bash
bazel test //test/lib/base/...
```

## Gotchas
- `Writer` is in `net_instaweb::` namespace (legacy), while `MessageHandler` is in `pagespeed::`.
- Only add to `string_util.h` if the functionality isn't available in `std::` or `absl::`.
