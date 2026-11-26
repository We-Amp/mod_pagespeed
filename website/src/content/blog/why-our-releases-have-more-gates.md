---
title: 'Why our releases have more gates now'
description: 'AI agents write, test and review the changes in every mod_pagespeed release. What a release must pass before it ships, and the release behind one gate.'
date: 2026-11-26
author: 'Otto van der Schaaf'
tags: ['releases', 'security', 'deep-dive', 'mod_pagespeed']
draft: false
---

mod_pagespeed 2.10.0 is the tenth release from 2.5.0 on 19 October. AI agents wrote its changes and tests, and reviewed the code. A person approves the signing and publishing of every release; agents do everything else, including site deploys, behind automated gates. Critical or challenging changes also get a person's review. A release is built once from a named commit, tested on those bytes, and signed and published only after that approval. If the commit or the files change after the test run, the release refuses to go out.

Most of the gates in that process were planned. One is there because a release went wrong without it, in a way you could have seen. This post lists both.

## What a release passes before it reaches you

- the full test tier on the release commit, sanitizer builds included ([the CI gates](https://github.com/We-Amp/mod_pagespeed/blob/master/docs/ci/release.md#the-ci-gates));
- a candidate run of the release lane on that exact commit, with every build, test and package smoke, whose sealed files the release then signs instead of building again (since 2.8.1, [how that works](https://github.com/We-Amp/mod_pagespeed/blob/master/docs/ci/release.md#adopting-a-candidate-run)). The Windows installer is the exception: the release builds it again around the sealed binaries;
- nine checks that the tag, the commit and every file hash still match that candidate;
- a green nightly in both product repositories, or a written reason for each red job;
- a person's approval on each signing and publishing step.

AI agents do the code review, with multiple independent lenses using top-tier models. A review can stop a change. It cannot let a release through: [the gates are scripts](/blog/is-mod-pagespeed-safe/#how-the-work-is-checked-and-where-ai-fits).

## A gate you could have seen being earned

**A red nightly before a tag (2.9.0).** The code was written by an agent and passed agent review.

- On 17 November at 03:47 UTC, the pagespeed-optimizer nightly failed on commit `d9996050d`, including its browser test job ([run 44012605598](https://github.com/We-Amp/pagespeed-optimizer/actions/runs/44012605598)).
- At 09:21 UTC, pagespeed-optimizer 2.9.0 was published from that same commit. In that release, clicking a URL in the URL Inspector of the pagespeed-optimizer console or workbench, on an nginx site, showed "No alternates found". The mod_pagespeed admin console was not affected.
- [pagespeed-optimizer 2.9.1](https://github.com/We-Amp/pagespeed-optimizer/releases/tag/optimizer-v2.9.1) shipped the fix on 18 November, and the container images got it with their 2.9.2 release.
- Now the latest nightly of both repositories has to be green before any tag, or each red job is looked at and the reason written down.

How the release train is run, and what an agent may not do on it, is in [the write-up on we-amp.com](https://we-amp.com/writing/ai-agents-release-train/).

## What a gate cannot catch

A gate checks what it was written to check. Our syscall filter for headless Chrome passed every soak we ran, and then the kernel killed the browser on a call it makes every few minutes: [our seccomp soak passed, then Chrome called mincore](/blog/our-seccomp-soak-passed-then-chrome-called-mincore/). More gates make a miss like that rarer. They do not rule it out.

## How often releases come

Ten releases in 35 days, usually four days apart; the gaps ran from two to six days. A fix waits for the next release, which so far has meant days. You do not have to take each one. The releases so far (UTC):

| Release | Published |
|---|---|
| 2.5.0 | 19 October 2026 |
| 2.5.2 | 22 October |
| 2.6.0 | 28 October |
| 2.7.0 | 1 November |
| 2.8.0 | 5 November |
| 2.8.1 | 9 November |
| 2.8.2 | 13 November |
| 2.9.0 | 17 November |
| 2.9.2 | 19 November |
| 2.10.0 | 23 November |

Skipping a release is fine. Read the "Action required when upgrading" section of each release's notes before you jump several versions ([release notes](/docs/release-notes/), [upgrade guide](/docs/upgrade/)).

## What it buys you

Security fixes are not worked on in public pull requests or issues. They ship in a regular release, through every channel at the same time. Nobody gets a fixed build before the public release, paying customers included ([security policy](https://github.com/We-Amp/mod_pagespeed/blob/master/SECURITY.md#fixes-and-announcements)).

You do not have to trust any of the above. Every release from 2.5.0 on carries `SHA256SUMS`, a detached signature of it, an SPDX SBOM and a VEX file. From 2.9.0 on it also carries a Sigstore bundle ([the 2.10.0 release](https://github.com/We-Amp/mod_pagespeed/releases/tag/module-v2.10.0)). Every release except 2.8.2 also has a signed source tag, such as `v2.10.0`.

Download `RELEASE-SIGNING-KEY.asc` from the root of the repository, and check that its fingerprint is `411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313`, as the security policy lists. Then verify against that key alone:

```bash
gpg --show-keys RELEASE-SIGNING-KEY.asc
gpg --dearmor < RELEASE-SIGNING-KEY.asc > release.gpg
gpgv --keyring ./release.gpg SHA256SUMS.asc SHA256SUMS \
  && sha256sum --check --ignore-missing SHA256SUMS
```

On macOS, use `shasum -a 256 --check --ignore-missing SHA256SUMS` for the last step. The full set of commands, including the Sigstore check, is in [Verifying a release](https://github.com/We-Amp/mod_pagespeed/blob/master/SECURITY.md#verifying-a-release).

To see which sealed build a release adopted, read its tag:

```bash
git init -q tag-check && cd tag-check
git fetch -q --depth 1 --filter=blob:none https://github.com/We-Amp/mod_pagespeed tag module-v2.10.0
git cat-file -p module-v2.10.0 | grep Release-Candidate
```

The three lines name the candidate run and the hashes of its seal and `SHA256SUMS`.

If something looks wrong in a release, open an issue on [GitHub](https://github.com/We-Amp/mod_pagespeed/issues). If it is a security problem, use the [security policy](/security/) instead.
