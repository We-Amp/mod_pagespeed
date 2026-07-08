# mod_pagespeed

**mod_pagespeed, maintained again.**

Google released its final version of mod_pagespeed in 2020. We-Amp picked it up. [**mod_pagespeed 1.15**](https://modpagespeed.com/1.1/) is the maintained continuation of the original open-source module — a drop-in replacement with the same configuration, the same filters, and the same behavior, plus ongoing security patches, the new [Cyclone Cache](https://modpagespeed.com/1.1/#cyclone), and direct support from the people who know the codebase best.

The project is led by [Otto van der Schaaf](https://github.com/oschaaf), Apache PageSpeed committer and IPMC member, with 360+ pull requests across the upstream codebase.

| | |
|---|---|
| **Install (Apache)** | [Quickstart →](https://modpagespeed.com/1.1/docs/getting-started/) |
| **Install (nginx)** | [Quickstart →](https://modpagespeed.com/1.1/docs/getting-started/) |
| **Install (IIS)** | [Quickstart →](https://modpagespeed.com/1.1/docs/getting-started/) |
| **Download packages** | [.deb / .rpm / .msi →](https://modpagespeed.com/download/) |
| **Upgrade from open-source** | [Migration guide →](https://modpagespeed.com/1.1/docs/upgrading-from-open-source/) |
| **Support** | [Email the maintainer →](https://modpagespeed.com/contact/) |

## What's in 1.15

- **Drop-in replacement.** Same configuration directives, same filters, same `mod_pagespeed.so` semantics. Your existing config keeps working.
- **Security patches** for known CVEs that accumulated against the archived upstream + much more. Hardened builds. Active supply chain management.
- **Cyclone Cache** — a new C++23 lock-free shared-memory cache that replaces the legacy file cache. No tuning required; warm-up is automatic.
- **First-class IIS** — native module for Windows Server 2019+ (IIS 10+, 64-bit only) with `.msi` installer.
- **Modern build** — Bazel-based, pre-built binaries for Debian/Ubuntu (amd64 + arm64), RHEL-family (x86_64 + aarch64), and Windows.
- **Direct maintainer support** included with every license.

## Currently shipping

| Platform | Status | Packages |
|---|---|---|
| **Apache** (amd64 + arm64) | GA | `.deb`, `.rpm`, `.so` |
| **nginx** (amd64 + arm64) | GA | signed apt/yum dynamic module `nginx-module-pagespeed` (`.deb`, `.rpm`) |
| **IIS** (Windows Server 2019+) | GA | `.msi` |
| **Envoy** | Experimental | experimental HTTP filter |

The nginx dynamic module ships prebuilt and signed for Debian 11/12/13 and Ubuntu 22.04/24.04 (amd64 + arm64), each pinned to that distribution's stock nginx version. Install via the signed apt/yum repository at [packages.modpagespeed.com](https://packages.modpagespeed.com). The Envoy HTTP filter is experimental — [contact us](https://modpagespeed.com/contact/) for setup guidance.

[Download and run →](https://modpagespeed.com/download/)

## About this repository

This repository exists as a public landing point for the mod_pagespeed project under We-Amp's stewardship. Active development happens in a separate repository; **all downloads, documentation, and support are at [modpagespeed.com](https://modpagespeed.com/1.1/)**.

For issues or questions about a running deployment, please [contact the maintainer](https://modpagespeed.com/contact/) — that's the fastest path to a response.

## License

mod_pagespeed 1.15 is distributed under the [Business Source License 1.1](https://modpagespeed.com/license/). Source publication is planned, with no date committed. You can install and run the module unlicensed to evaluate it — it fully optimizes and simply adds an `X-PageSpeed-Warn: unlicensed` response header (plus an admin-console notice and a startup-log warning). A commercial license is required for production use. See [pricing](https://modpagespeed.com/pricing/) for license details.

## Background

mod_pagespeed was created at Google in 2010 and powered web performance optimization across hundreds of thousands of sites. We-Amp's role is documented in primary sources: the [Apache Incubator PageSpeed proposal](https://cwiki.apache.org/confluence/display/INCUBATOR/PageSpeedProposal) lists We-Amp B.V. as a founding committer organization alongside Google, and [Google's 2013 ngx_pagespeed announcement](https://developers.googleblog.com/en/speed-up-your-sites-with-pagespeed-for-nginx/) named We-Amp among the module's contributors. After Google archived the project, We-Amp B.V. continued active development in the Apache PageSpeed incubator project and after that under the mod_pagespeed 1.15 line, alongside a ground-up rewrite, [ModPageSpeed 2.0](https://modpagespeed.com/).

Learn more about We-Amp's open-source work: [we-amp.com/open-source/](https://we-amp.com/open-source/).
