---
title: 'Cyclone vs the file cache: a memory-mapped cache benchmark'
description: 'Cache benchmark: Cyclone (memory-mapped, in-process RAM tier) vs the classic file-per-entry cache, across concurrency, traffic, latency tails and eviction.'
date: 2026-07-04
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['performance', 'benchmarks', 'cache']
product: '2.0'
faq:
  - q: 'Is Cyclone faster than the old file-per-entry cache?'
    a: 'Under concurrency, eviction pressure, and on realistic server storage, yes: 4x to 6x on throughput in our tests, with a tighter latency tail. On an idle, over-provisioned cache backed by a fast NVMe SSD, the two are close and the file cache can edge ahead.'
  - q: 'Does the mod_pagespeed module benefit from Cyclone?'
    a: 'Yes. The module has shipped Cyclone since 1.15 and reads from it zero-copy: a cache hit is a pointer into the mapped file rather than a system call plus a heap copy. That read path is exactly what this benchmark measures.'
  - q: 'Will a faster cache make my site load faster?'
    a: 'It helps most when the cache is on the hot path: high concurrency, lots of already-optimized assets, and cache eviction under load. On a lightly loaded site the cache was rarely the bottleneck, so the end-to-end gain is smaller.'
  - q: 'Did a later rework change these results?'
    a: 'Yes, for the read path. In July 2026 we made concurrent reads lock-free and dropped a per-write sync, then re-measured on the same machine. In the read-bound regime, in the configuration mod_pagespeed serves with (the RAM tier off by default), Cyclone now serves about 10x the file-per-entry cache — about 5x of that the rework itself, measured against the previous Cyclone on the same box. It also pulled the p999 tail under memory pressure down from 67-134 ms to 8-17 ms and turned a 512 MB memory-pressure case that used to lose to the file cache into a win. The original numbers in this post are unchanged and predate the rework, so they understate current Cyclone.'
  - q: 'How large should I size the cache?'
    a: 'Aim to keep the hot working set in RAM. A memory-mapped cache is demand-paged, so the cache can be larger than physical memory; only the pages in active use need to be resident. Push the working set itself far past RAM and it pages to disk, about 1.8x slower in our test, and still faster than the file cache at that size. The file cache depends on the OS page cache the same way, so this holds for any disk-backed cache.'
---

Every cache backend behind mod_pagespeed sits under one interface, so the optimizer above it never changes. That makes a direct question easy to ask: how much does the storage engine itself matter? We put **Cyclone**, the memory-mapped cache both parts of mod_pagespeed share, against the classic file-per-entry disk cache it replaced, and measured both across concurrency, realistic mixed traffic, latency tails, and eviction, on two very different machines.

The most useful result is a trend. Cyclone's advantage is smallest on a laptop with a fast NVMe SSD and grows to **4x-6x** on the kind of storage a real server runs on. The faster the disk, the more the old design's per-file cost stays hidden.

> **Update — 2026-07-14:** Since this was published we reworked Cyclone's read and write paths and re-measured on the same machine. Concurrent reads are now lock-free — a cache hit is a pointer into the mapped file with no lock handoff, so read throughput scales with cores instead of flattening under contention — and the write path dropped a per-write sync that had been serializing cache fills. The headline: in the read-bound regime, in the configuration mod_pagespeed serves with — the in-process RAM tier off by default, so reads come straight from the memory-mapped volume, the reworked Cyclone serves about 10x the read throughput of the file-per-entry cache. About 5x of that is the rework itself, measured against the previous Cyclone on the same box and config — the cleanest attribution of the change. It also flipped Cyclone's worst documented case, behavior under memory pressure, from a loss into a win, and pulled its latency tail down sharply. The controlled before-and-after, with numbers and caveats, is in [The read-path rework](#the-read-path-rework-a-controlled-re-measure) below; the original run is left in place, unchanged, for comparison.

<figure class="not-prose" style="margin:2.2rem 0">
<svg viewBox="0 0 800 470" role="img" aria-label="Read throughput before and after the rework across four memory regimes" style="width:100%;height:auto;font-family:ui-sans-serif,system-ui,sans-serif">
<line x1="60" y1="400.0" x2="782" y2="400.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="404.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">0</text>
<line x1="60" y1="335.6" x2="782" y2="335.6" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="339.6" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">40.0k</text>
<line x1="60" y1="271.2" x2="782" y2="271.2" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="275.2" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">80.0k</text>
<line x1="60" y1="206.8" x2="782" y2="206.8" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="210.8" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">120k</text>
<line x1="60" y1="142.4" x2="782" y2="142.4" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="146.4" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">160k</text>
<line x1="60" y1="78.0" x2="782" y2="78.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="51" y="82.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">200k</text>
<text x="60" y="38" font-size="12" fill="currentColor" fill-opacity="0.55" font-family="ui-monospace,monospace">throughput (ops/s) · RAM tier off</text>
<rect x="60" y="50" width="13" height="13" rx="3" fill="#12A594" fill-opacity="1.0"/>
<text x="79" y="61" font-size="12.5" fill="currentColor" fill-opacity="0.78">Cyclone (new)</text>
<rect x="210" y="50" width="13" height="13" rx="3" fill="#12A594" fill-opacity="0.4"/>
<text x="229" y="61" font-size="12.5" fill="currentColor" fill-opacity="0.78">Cyclone (previous)</text>
<rect x="400" y="50" width="13" height="13" rx="3" fill="#C08457" fill-opacity="1.0"/>
<text x="419" y="61" font-size="12.5" fill="currentColor" fill-opacity="0.78">File cache</text>
<rect x="90.2" y="353.1" width="34" height="46.9" rx="4" fill="#12A594" fill-opacity="0.4"/>
<text x="107.2" y="347.1" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">29.1k</text>
<rect x="133.2" y="152.2" width="34" height="247.8" rx="4" fill="#12A594" fill-opacity="1.0"/>
<text x="150.2" y="146.2" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">154k</text>
<rect x="176.2" y="376.4" width="34" height="23.6" rx="4" fill="#C08457" fill-opacity="1.0"/>
<text x="193.2" y="370.4" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">14.7k</text>
<text x="150.2" y="430" text-anchor="middle" font-size="14.5" font-weight="650" fill="currentColor">8 GB</text>
<text x="150.2" y="448" text-anchor="middle" font-size="12" fill="currentColor" fill-opacity="0.6">read-bound · phys RAM</text>
<rect x="270.8" y="356.8" width="34" height="43.2" rx="4" fill="#12A594" fill-opacity="0.4"/>
<text x="287.8" y="350.8" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">26.9k</text>
<rect x="313.8" y="173.2" width="34" height="226.8" rx="4" fill="#12A594" fill-opacity="1.0"/>
<text x="330.8" y="167.2" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">141k</text>
<rect x="356.8" y="376.4" width="34" height="23.6" rx="4" fill="#C08457" fill-opacity="1.0"/>
<text x="373.8" y="370.4" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">14.7k</text>
<text x="330.8" y="430" text-anchor="middle" font-size="14.5" font-weight="650" fill="currentColor">1 GB</text>
<text x="330.8" y="448" text-anchor="middle" font-size="12" fill="currentColor" fill-opacity="0.6">read-bound · phys RAM</text>
<rect x="451.2" y="392.3" width="34" height="7.7" rx="4" fill="#12A594" fill-opacity="0.4"/>
<text x="468.2" y="386.3" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">4.8k</text>
<rect x="494.2" y="366.7" width="34" height="33.3" rx="4" fill="#12A594" fill-opacity="1.0"/>
<text x="511.2" y="360.7" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">20.7k</text>
<rect x="537.2" y="378.7" width="34" height="21.3" rx="4" fill="#C08457" fill-opacity="1.0"/>
<text x="554.2" y="372.7" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">13.2k</text>
<text x="511.2" y="430" text-anchor="middle" font-size="14.5" font-weight="650" fill="currentColor">512 MB</text>
<text x="511.2" y="448" text-anchor="middle" font-size="12" fill="currentColor" fill-opacity="0.6">under pressure · phys RAM</text>
<rect x="631.8" y="395.4" width="34" height="4.6" rx="4" fill="#12A594" fill-opacity="0.4"/>
<text x="648.8" y="389.4" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">2.8k</text>
<rect x="674.8" y="385.2" width="34" height="14.8" rx="4" fill="#12A594" fill-opacity="1.0"/>
<text x="691.8" y="379.2" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">9.2k</text>
<rect x="717.8" y="380.6" width="34" height="19.4" rx="4" fill="#C08457" fill-opacity="1.0"/>
<text x="734.8" y="374.6" text-anchor="middle" font-size="12" font-weight="600" fill="currentColor" fill-opacity="0.82" font-family="ui-monospace,monospace">12.0k</text>
<text x="691.8" y="430" text-anchor="middle" font-size="14.5" font-weight="650" fill="currentColor">384 MB</text>
<text x="691.8" y="448" text-anchor="middle" font-size="12" fill="currentColor" fill-opacity="0.6">severe · phys RAM</text>
<line x1="60" y1="400" x2="782" y2="400" stroke="currentColor" stroke-opacity="0.28"/>
</svg>
<figcaption style="text-align:center;font-size:0.82rem;color:var(--color-text-muted);margin-top:0.6rem">Read throughput across four memory regimes, same 64-core workstation, in-process RAM tier off (mod_pagespeed's default — reads come straight from the mapped volume). Benchmark: 40,000 keys of ~11 KB, 8 worker threads, a Zipfian read/write/delete mix; average of two runs. Faded bars are the previous Cyclone, solid teal the reworked build, brown the file cache — the lead holds while the working set is resident (left) and narrows under memory pressure (right).</figcaption>
</figure>

## Two engines, one interface

**Cyclone** keeps one memory-mapped file per volume. A read returns a pointer into mapped memory, with no `open`/`read`/`close` and no heap copy. An in-process RAM tier holds the hot set, eviction runs inline as entries are written, and the admission policy resists one-hit-wonders flushing the working set. Cyclone is open source: [github.com/We-Amp/cyclone-cache](https://github.com/We-Amp/cyclone-cache); we have since measured it as an LLM KV-cache storage tier against LMDB in [Cyclone as an LLM KV-cache tier](/blog/cyclone-kv-cache-tier-vs-lmdb/).

**The file-per-entry cache** stores one file per entry. Every read is `open`, then `read`, then `close`, plus a copy into a fresh buffer; every write creates a file. Eviction is deferred to a periodic janitor that walks the whole cache directory, an O(n) scan that grows with the file count.

Everything below is the median of repeated runs on quiesced machines. Where the file cache wins, the numbers say so.

## Concurrency: the file cache stops scaling

A live server answers many requests at once. Ramping concurrent readers from 1 to 64 against a warm cache, Cyclone's mapped reads carry no per-operation lock or system call, so aggregate throughput climbs with cores. The file cache peaks near 4-8 threads, then declines as lock and system-call contention take over.

<figure class="not-prose" style="margin:2.2rem 0">
<svg viewBox="0 0 800 440" role="img" aria-label="Aggregate reads per second versus concurrent threads: Cyclone keeps scaling, the file cache plateaus" style="width:100%;height:auto;font-family:ui-sans-serif,system-ui,sans-serif">
<line x1="64" y1="386.0" x2="736" y2="386.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="390.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">0</text>
<line x1="64" y1="322.0" x2="736" y2="322.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="326.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">300k</text>
<line x1="64" y1="258.0" x2="736" y2="258.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="262.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">600k</text>
<line x1="64" y1="194.0" x2="736" y2="194.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="198.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">900k</text>
<line x1="64" y1="130.0" x2="736" y2="130.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="134.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">1.2M</text>
<line x1="64" y1="66.0" x2="736" y2="66.0" stroke="currentColor" stroke-opacity="0.11"/>
<text x="55" y="70.0" text-anchor="end" font-size="12.5" fill="currentColor" fill-opacity="0.55">1.5M</text>
<text x="64" y="26" font-size="12" fill="currentColor" fill-opacity="0.55" font-family="ui-monospace,monospace">aggregate reads/sec · workstation · 10 KB objects</text>
<line x1="64" y1="43" x2="86" y2="43" stroke="#12A594" stroke-width="3"/>
<circle cx="75" cy="43" r="3.5" fill="#12A594"/>
<text x="94" y="47" font-size="12.5" fill="currentColor" fill-opacity="0.78">Cyclone</text>
<line x1="204" y1="43" x2="226" y2="43" stroke="#C08457" stroke-width="3"/>
<circle cx="215" cy="43" r="3.5" fill="#C08457"/>
<text x="234" y="47" font-size="12.5" fill="currentColor" fill-opacity="0.78">File cache</text>
<text x="64.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">1</text>
<text x="176.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">2</text>
<text x="288.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">4</text>
<text x="400.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">8</text>
<text x="512.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">16</text>
<text x="624.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">32</text>
<text x="736.0" y="410" text-anchor="middle" font-size="12.5" fill="currentColor" fill-opacity="0.6">64</text>
<text x="400.0" y="430" text-anchor="middle" font-size="12" fill="currentColor" fill-opacity="0.55">concurrent threads</text>
<polyline points="64.0,374.7 176.0,362.6 288.0,342.6 400.0,317.4 512.0,308.0 624.0,316.0 736.0,328.7" fill="none" stroke="#C08457" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>
<circle cx="64.0" cy="374.7" r="3.4" fill="#C08457"/>
<circle cx="176.0" cy="362.6" r="3.4" fill="#C08457"/>
<circle cx="288.0" cy="342.6" r="3.4" fill="#C08457"/>
<circle cx="400.0" cy="317.4" r="3.4" fill="#C08457"/>
<circle cx="512.0" cy="308.0" r="3.4" fill="#C08457"/>
<circle cx="624.0" cy="316.0" r="3.4" fill="#C08457"/>
<circle cx="736.0" cy="328.7" r="3.4" fill="#C08457"/>
<polyline points="64.0,376.6 176.0,368.7 288.0,350.5 400.0,313.3 512.0,242.0 624.0,122.3 736.0,118.7" fill="none" stroke="#12A594" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>
<circle cx="64.0" cy="376.6" r="3.4" fill="#12A594"/>
<circle cx="176.0" cy="368.7" r="3.4" fill="#12A594"/>
<circle cx="288.0" cy="350.5" r="3.4" fill="#12A594"/>
<circle cx="400.0" cy="313.3" r="3.4" fill="#12A594"/>
<circle cx="512.0" cy="242.0" r="3.4" fill="#12A594"/>
<circle cx="624.0" cy="122.3" r="3.4" fill="#12A594"/>
<circle cx="736.0" cy="118.7" r="3.4" fill="#12A594"/>
<text x="730.0" y="109.7" text-anchor="end" font-size="12.5" font-weight="600" fill="#12A594" font-family="ui-monospace,monospace">1.3M</text>
<text x="730.0" y="346.7" text-anchor="end" font-size="12.5" font-weight="600" fill="#C08457" font-family="ui-monospace,monospace">269k</text>
</svg>
<figcaption style="text-align:center;font-size:0.82rem;color:var(--color-text-muted);margin-top:0.6rem">Aggregate reads per second as concurrent readers climb from 1 to 64 (workstation, 10 KB objects). Cyclone's lock-free mapped reads keep scaling with cores; the file cache peaks near 8-16 threads, then declines as system-call and lock contention take over.</figcaption>
</figure>

Aggregate reads per second at 64 threads, laptop (fast NVMe SSD):

| Object size | File cache | Cyclone   | Improvement |
| ----------- | ---------- | --------- | ----------- |
| 100 B       | 601,358    | 2,715,255 | 4.5x        |
| 10 KB       | 592,166    | 2,090,796 | 3.5x        |
| 1 MB        | 20,897     | 84,704    | 4.1x        |

And on the workstation (realistic server storage):

| Object size | File cache | Cyclone   | Improvement |
| ----------- | ---------- | --------- | ----------- |
| 100 B       | 273,742    | 1,533,871 | 5.6x        |
| 10 KB       | 268,719    | 1,252,859 | 4.7x        |
| 1 MB        | 41,184     | 164,090   | 4.0x        |

One case runs the other way. At a single thread, the file cache can be faster, on the laptop it actually wins for 10 KB and 1 MB objects, because a warm read is just a page-cache copy and nothing more. That edge disappears the moment real concurrency arrives. The next section explains why.

## Both fit in RAM. Why is memory-mapped still faster?

A fair objection: the readers above run against a warm cache, so the file cache's bytes are already in the OS page cache. They are in RAM. Both engines serve from memory, and no request touches the SSD. So why is the file cache 4x-6x slower at 64 threads?

Because a file-cache hit is not free even when nothing reads the disk. Every hit still pays `open`, `read`, `close`: several system calls, each a round trip across the user/kernel boundary, plus a `copy_to_user` that physically copies the object out of the page cache into your buffer. A memory-mapped hit pays none of that. It is a load from a page already resident in RAM, with at most a one-time minor fault the first time a page is touched.

For large objects the copy is most of the cost. For small ones the real tax is contention: those same system calls force every thread to touch shared kernel structures, the directory and file dentry locks, the inode, the process file-descriptor table, and under many cores those cache lines ping-pong between them. That is why the file cache tops out around 4-8 threads and then gets slower as you add more, while the memory-mapped path, read-only shared pages with no cross-core writes, keeps scaling with cores.

The honest tell is the single-thread number, where the file cache can win. With no concurrency there is nothing to serialize, and a lone warm `read` plus copy is a very tight path, tighter than mmap's first-touch fault and TLB setup. The advantage only appears under concurrency, which is precisely the server workload that matters.

## The read-path rework: a controlled re-measure

This is a later addition. After publishing, we made concurrent reads lock-free and dropped a per-write sync, then re-ran one benchmark to measure just those changes. To isolate the read path we turned the in-process RAM tier off and swept physical memory instead, so these numbers are not a drop-in replacement for the "Realistic traffic" figures further down — that run had the RAM tier on. Read this as a separate, read-path-isolated measurement on the same workstation, averaged over two runs.

Lead with the number that matters in production. In the read-bound regime, with Cyclone's in-process RAM tier off — which is how mod_pagespeed serves by default: reads come straight from the memory-mapped volume, since a per-process RAM copy only duplicates what the OS page cache already holds — the reworked Cyclone runs about 10x the file-per-entry cache: roughly 154,000 ops/s against 14,600. That is a production-configuration number, not a thumb on the scale. What conditions it is the regime, not the tier: it holds while the working set is resident and reads dominate, and it narrows as memory pressure rises, as the chart above shows. (The original "realistic" run further down used a 128 MB in-process tier — not mod_pagespeed's default — which short-circuits some reads before they reach the mapped path, one reason it posts a smaller 4.1x.)


How much of that 10x is the rework itself, versus the file cache's inherent per-read syscall cost? Hold the configuration fixed and run the previous and current Cyclone builds back to back on the same box: about 5x, 154,000 ops/s against 29,000. Only the code changed, so that is the cleanest attribution of the lock-free read path — the pure Cyclone-over-Cyclone gain, with no help from the comparison target.

The tail moved further than the average. Under memory pressure the previous build's p999 read latency reached 67 to 134 ms; the reworked path holds p999 in the 8 to 17 ms band, and around 2.1 ms in the read-bound regime. Dropping the per-write sync took a class of multi-millisecond stalls out of the distribution — the kind of worst case an operator feels as unpredictable behavior when the box is under load.

The rework also flipped a case the original run flagged as a loss. At a 512 MB physical cap the previous Cyclone lost to the file cache outright (4,756 versus 13,230 ops/s); the reworked build reverses that to a win (20,686 versus 13,230), about 1.56x. We call it out because our earlier numbers documented that loss.

**Two honest limits.** It does not close the gap everywhere. Push the physical cap down to 384 MB, with the working set well beyond RAM, and the file cache still wins: 9,175 versus 12,032, or Cyclone at 0.76x. And throughout this sweep Cyclone posts a lower hit rate than the file cache, about 79% versus 88% — Cyclone holds its size budget while the file cache admits everything and lets its footprint grow, so part of the file cache's standing under pressure is simply that it is caching more.


A few limits worth stating plainly. Each point is the average of two runs, not a variance-controlled result — read it as a clean controlled before-and-after, not a fleet study. It is one machine, one object-size profile (about 11 KB), and one access distribution; the crossover point where the file cache overtakes will move on other hardware and workloads. And only this one benchmark was re-run on the new build — the concurrency, eviction, overflow, and sizing sections below still reflect the previous Cyclone, so they understate it.

## Realistic traffic, under pressure

_The numbers in this section are the original run, measured with the in-memory RAM tier on — a different configuration from the read-path re-measure above. They stand as first published._

A Zipfian mix of reads, writes, and deletes over a heavy-tailed object-size distribution, with a working set about twice the cache size so eviction runs continuously, the way it does on a busy site. Eight worker threads, a 1 GB cache with a 128 MB RAM tier.

| Metric (workstation) | Cyclone    | File cache |
| -------------------- | ---------- | ---------- |
| Throughput           | 111k ops/s | 27k ops/s  |
| p99 latency          | 1.0 ms     | 4.2 ms     |
| p999 tail            | 2.1 ms     | 8.4 ms     |

On this storage Cyclone runs **4.1x** the throughput with roughly a quarter of the p999 tail. The file cache often posts a higher hit rate, because it admits everything and lets its footprint grow to do it, but it serves what it holds far more slowly. On the laptop's fast NVMe SSD the throughput gap narrows to 1.6x, which is the point: the slower and more realistic the disk, the wider the margin.

The full latency distribution — the reworked Cyclone against the previous build and the file cache, in both the read-bound and under-pressure regimes — is in the [interactive data explorer](/cyclone-benchmark/). The curves make the tail difference obvious in a way percentiles alone do not.

## The through-line: a slower disk amplifies the win

Here is every headline result as a ratio, Cyclone over the file cache, for both machines. The laptop is the conservative case, with cheap per-file cost, so the file cache looks its best. The workstation's realistic storage is closer to production. In every pairing, the slower disk makes the gap larger.

| Test                       | Laptop (fast SSD) | Workstation (realistic) |
| -------------------------- | ----------------- | ----------------------- |
| Concurrency, 64 threads    | 4.5x              | 5.6x                    |
| Realistic, under pressure  | 1.6x              | 4.1x                    |
| Eviction sweep, 100k files | 2.0x              | 6.0x                    |
| Overflow write rate        | 2.9x              | 5.6x                    |

The practical lesson: benchmarks run on developer laptops with fast local SSDs understate what a memory-mapped cache buys in production.

## Eviction: a flat line vs. a stall

Stream 100,000 files into a small cache and watch write throughput batch by batch. Cyclone evicts inline and holds steady. The file cache defers to its janitor; each time the scan fires, writers stall, and the walk gets more expensive as the file count climbs. On the workstation the file cache finished the same sweep **6x** slower.

There is a real-world version of this. The file cache also writes a small file for every miss it remembers, so every not-found input URL becomes an inode the janitor later has to walk. On a busy server that churn inflates the very scan above. Cyclone's single-file design and selective admission do not accumulate that debris.

## Staying inside the cap

Write far more than a 16 MB cache holds, and the difference in discipline shows. Cyclone evicts inline and holds the cap at 15.5 MB. The file cache's deferred janitor lets the directory balloon well past the limit before it catches up: 5x over on the workstation, 9x on the laptop. Predictable disk usage is worth as much to an operator as raw speed.

## Sizing: keep the hot working set in RAM

A memory-mapped cache is demand-paged, so only the pages you actually touch need to be resident, the hot working set, not the whole file. The cache can safely be larger than RAM: the kernel keeps hot pages in memory and pages cold ones in on access. You pay for extra disk reads only when the working set itself outgrows RAM. We measured that deliberately, a 4 GB cache in a 2 GB budget with a working set about twice the cache, and throughput dropped roughly 1.8x. That is graceful degradation, not a cliff, and even while paging, Cyclone still ran 1.5x-3.5x faster than the file cache at the same oversized size.

| Machine     | Engine     | Fits in RAM (ops/s) | Oversized, paging to disk (ops/s) |
| ----------- | ---------- | ------------------- | --------------------------------- |
| Laptop      | Cyclone    | 132,504             | 69,524                            |
| Laptop      | File cache | 78,524              | 46,993                            |
| Workstation | Cyclone    | 114,974             | 65,161                            |
| Workstation | File cache | 27,142              | 18,340                            |

This is not a tax Cyclone introduces. The file cache is fast for exactly the same reason, its bytes live in the OS page cache, so any disk-backed cache slows down once its hot set spills past memory; the file cache degrades here too, and from a lower starting point. The guidance is unchanged and cheap: size RAM to your hot working set, not to the entire cache. It is spelled out in the [production deployment guide](/docs/production-deployment/).

## Where this lands in the product {#where-this-lands-in-the-products}

The module already reads from Cyclone with no copy: a hit is a pointer into the mapped file, not a system call and a buffer allocation, which is exactly the read path these numbers measure — and from v1.15.0+r19 it can extend that to serving, behind the opt-in `CycloneZeroCopy` switch. The optimizer worker carries the memory-mapped path further into the request by default, serving cached bytes toward the socket without an intermediate copy. How it does that, streaming large hits with kernel `sendfile` straight from the shared cache file, copying small ones because the syscall would cost more, and staying correct while the cache changes underneath a slow client, is its own deep-dive: [zero-copy serving between nginx and the worker](/blog/memory-mapped-cache-zero-copy-serving/). If you run a file-cache configuration under real concurrency or eviction pressure, the update is worth taking.

A fair boundary on the claim: this is a cache benchmark, not an end-to-end page-serving benchmark. It shows the cache component is faster, not that a page renders some fixed percentage sooner. On most requests, origin fetch and optimization work dominate the clock. The cache win reaches the visitor precisely when a server is busy, with many concurrent hits, a large working set, and eviction running hot. That is also when a server most needs the help.

We checked that boundary directly. Against the last mod_pagespeed release of the Google era — 1.13.35.2, which serves from the file-per-entry cache — the module with Cyclone holds parity on end-to-end HTML serving at product defaults: the same throughput within noise (about 3,200 to 3,300 requests per second either way in a keepalive load test), the same CPU per request, and the same optimized output. On a normal page workload the cache is not the bottleneck, so a faster cache does not move that number — thirteen years of changes plus the Cyclone rebuild did not regress it.

The read path only became the bottleneck under a harsher regime: a working set far larger than the hot set, hammered by hundreds of concurrent random-access readers. There the old lock-bound read path serialized and fell behind — under that load it trailed even the file-per-entry design it replaced. That is the regression this rework was built to remove, and the before-and-after above is its engine-level measure of the fix.

## Frequently asked questions

**Is Cyclone faster than the old file-per-entry cache?** Under concurrency, eviction pressure, and on realistic server storage, yes: 4x to 6x on throughput in our tests, with a tighter latency tail. On an idle, over-provisioned cache backed by a fast NVMe SSD, the two are close and the file cache can edge ahead.

**Does the mod_pagespeed module benefit from Cyclone?** Yes. The module has shipped Cyclone since 1.15 and reads from it zero-copy: a cache hit is a pointer into the mapped file rather than a system call plus a heap copy. That read path is exactly what this benchmark measures.

**Will a faster cache make my site load faster?** It helps most when the cache is on the hot path: high concurrency, lots of already-optimized assets, and cache eviction under load. On a lightly loaded site the cache was rarely the bottleneck, so the end-to-end gain is smaller.

**How large should I size the cache?** Aim to keep the hot working set in RAM. A memory-mapped cache is demand-paged, so the cache can be larger than physical memory; only the pages in active use need to be resident. Push the working set itself far past RAM and it pages to disk, about 1.8x slower in our test, and still faster than the file cache at that size. The file cache depends on the OS page cache the same way, so this holds for any disk-backed cache.

**Did a later rework change these results?** Yes, for the read path. In July 2026 we made concurrent reads lock-free and dropped a per-write sync, then re-measured on the same machine. In the read-bound regime, in the configuration mod_pagespeed serves with (the RAM tier off by default), Cyclone now serves about 10x the file-per-entry cache — about 5x of that the rework itself, measured against the previous Cyclone on the same box. It also pulled the p999 tail under memory pressure down from 67-134 ms to 8-17 ms and turned a 512 MB memory-pressure case that used to lose to the file cache into a win. The original numbers in this post are unchanged and predate the rework, so they understate current Cyclone. See the read-path rework section above for the controlled before-and-after and its caveats.

## The machines

Two deliberately different machines. The specs that matter for a cache benchmark are core count (concurrency), memory (the RAM tier and mapping headroom), and above all the storage stack and filesystem, which set the per-file system-call cost.

- **Laptop.** Apple-silicon, 10 cores, 16 GB, fast NVMe SSD (native APFS, multi-GB/s sequential reads). A very fast core and low-latency local flash, but few cores. This is where per-file cost is cheapest, so it is the conservative case for every filesystem claim here.
- **Workstation.** 64-core x86, 96 GB, ext4 on virtualized storage. Many threads draw the clean concurrency curve, and the virtualized storage adds the kind of filesystem-metadata overhead a production server actually pays.

Explore the numbers yourself, with per-machine toggles and the full latency distribution, in the [data explorer](/cyclone-benchmark/).

---

_In memory of Alan M. Carroll._ Some of Cyclone's design grew out of conversations with Alan, known to the [Apache Traffic Server](https://trafficserver.apache.org/) community as _SolidWallOfCode_ and among the people who understood its architecture most deeply. His thinking about how a cache lays bytes out on disk and reclaims space without stalling helped shape how we thought about ours, and he shared it generously. We remember him with gratitude. [Alan's memorial at the Apache Software Foundation](https://apache.org/memorials/alan_m_carroll.html).
