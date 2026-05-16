# mod_pagespeed

**mod_pagespeed, maintained again.**

Google released its final version of mod_pagespeed in 2020. We-Amp picked it up. [**mod_pagespeed 1.1**](https://modpagespeed.com/1.1/) is the maintained continuation of the original open-source module — a drop-in replacement with the same configuration, the same filters, and the same behavior, plus ongoing security patches, the new [Cyclone Cache](https://modpagespeed.com/1.1/#cyclone), and direct support from the people who know the codebase best.

The project is led by [Otto van der Schaaf](https://github.com/oschaaf), Apache PageSpeed committer and IPMC member, with 360+ pull requests across the upstream codebase.

| | |
|---|---|
| **Install (Apache)** | [Quickstart →](https://modpagespeed.com/1.1/docs/getting-started/) |
| **Install (IIS)** | [Quickstart →](https://modpagespeed.com/1.1/docs/getting-started/) |
| **Download packages** | [.deb / .rpm / .msi →](https://modpagespeed.com/download/) |
| **Upgrade from open-source** | [Migration guide →](https://modpagespeed.com/1.1/docs/upgrading-from-open-source/) |
| **Pricing** | [$49/server/month — 14-day free trial →](https://modpagespeed.com/pricing/) |
| **Support** | [Email the maintainer →](https://modpagespeed.com/contact/) |

## What's in 1.1

- **Drop-in replacement.** Same configuration directives, same filters, same `mod_pagespeed.so` semantics. Your existing config keeps working.
- **Security patches** for known CVEs that accumulated against the archived upstream.
- **Cyclone Cache** — a new C++23 lock-free shared-memory cache that replaces the legacy file cache. No tuning required; warm-up is automatic.
- **First-class IIS** — native module for Windows Server 2019 / 2022 with `.msi` installer.
- **Modern build** — Bazel-based, pre-built binaries for Debian/Ubuntu (amd64 + arm64), RHEL-family (x86_64 + aarch64), and Windows.
- **Direct maintainer support** included with every license.

## Currently shipping

| Platform | Status | Packages |
|---|---|---|
| **Apache** (amd64 + arm64) | GA | `.deb`, `.rpm`, `.so` |
| **IIS** (Windows Server 2019/2022) | GA | `.msi` |
| **nginx** | Coming soon | — |
| **Envoy** | Coming soon | — |

To be notified when nginx and Envoy ship, [sign up on the download page](https://modpagespeed.com/download/).

## About this repository

This repository exists as a public landing point for the mod_pagespeed project under We-Amp's stewardship. Active development happens in a separate repository; **all downloads, documentation, and support are at [modpagespeed.com](https://modpagespeed.com/1.1/)**.

For issues or questions about a running deployment, please [contact the maintainer](https://modpagespeed.com/contact/) — that's the fastest path to a response.

## License

mod_pagespeed 1.1 is distributed under the [Business Source License 1.1](https://modpagespeed.com/license/). The first 14 days are a free trial — full features, automatic expiration. See [pricing](https://modpagespeed.com/pricing/) for license details.

## Background

mod_pagespeed was created at Google in 2010 and powered web performance optimization across hundreds of thousands of sites. After Google archived the project, We-Amp B.V. — a Dutch company founded by the former maintainer — continued active development under the mod_pagespeed 1.1 line, alongside a ground-up rewrite, [ModPageSpeed 2.0](https://modpagespeed.com/).

Learn more about We-Amp's open-source work: [we-amp.com/open-source/](https://we-amp.com/open-source/).
