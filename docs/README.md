# PageSpeed WASM Documentation

> Documentation for the PageSpeed WASM proxy filter and optimization service.

## Quick Links

| Document | Description |
|----------|-------------|
| [PAGESPEED_WASM_PLAN.md](../PAGESPEED_WASM_PLAN.md) | Master implementation plan |
| [WASM_ARCHITECTURE.md](WASM_ARCHITECTURE.md) | Technical architecture overview |
| [WASM_HTML_PARSER.md](WASM_HTML_PARSER.md) | HTML parser design for WASM |
| [BUSINESS_MODEL.md](BUSINESS_MODEL.md) | Monetization and go-to-market strategy |
| [DECISIONS.md](DECISIONS.md) | Key technical decisions and rationale |
| [ROADMAP.md](ROADMAP.md) | Timeline and milestones |

## Project Overview

PageSpeed WASM is a modern web performance optimization solution that runs as a proxy-wasm filter across multiple platforms (Envoy, nginx, Apache Traffic Server).

### Architecture

```
┌─────────────────────┐     ┌─────────────────────┐
│  WASM Edge Filter   │────▶│ Optimization Service │
│  (15-25 MB)         │     │ (Native C++)         │
│                     │     │                      │
│  - HTML parsing     │     │  - Image processing  │
│  - URL rewriting    │     │  - CSS/JS minify     │
│  - Tag injection    │     │  - Critical CSS      │
│  - Caching (Redis)  │     │  - Caching (Cyclone) │
└─────────────────────┘     └──────────────────────┘
```

### Target Market

- SME websites and WordPress users
- a freemium pricing model
- Self-hosted option for enterprise

### Key Differentiators

1. **Multi-platform**: Works on Envoy, nginx, ATS
2. **Self-hosted option**: For compliance-sensitive customers
3. **Active development**: Google abandoned mod_pagespeed
4. **WordPress first**: Deep CMS integration

## Getting Started

### For Developers

1. Read [WASM_ARCHITECTURE.md](WASM_ARCHITECTURE.md) for technical overview
2. Review [DECISIONS.md](DECISIONS.md) for design rationale
3. Check [ROADMAP.md](ROADMAP.md) for current priorities

### For Business

1. Read [BUSINESS_MODEL.md](BUSINESS_MODEL.md) for strategy
2. Review [ROADMAP.md](ROADMAP.md) for timeline
3. Check [PAGESPEED_WASM_PLAN.md](../PAGESPEED_WASM_PLAN.md) for full plan

## Code Locations

| Component | Path | Status |
|-----------|------|--------|
| Proto definitions | `pagespeed/service/pagespeed_service.proto` | Created |
| Service BUILD | `pagespeed/service/BUILD` | Created |
| WASM components | `pagespeed/wasm/` | Planned |
| WordPress plugin | `integrations/wordpress/` | Planned |

## Document Conventions

- **Decision Records**: Use DECISIONS.md format
- **Architecture**: Use C4 model diagrams
- **API Docs**: Proto files are source of truth
- **Roadmap**: Update weekly during active development

## Contributing

1. Read existing documentation first
2. Follow decision-making process in DECISIONS.md
3. Update ROADMAP.md when completing milestones
4. Keep WASM_ARCHITECTURE.md current with changes

## Contact

- GitHub Issues: For bugs and feature requests
- Discussions: For questions and ideas
