---
title: 'Our seccomp soak passed. Then Chrome called mincore'
description: 'A log-only systemd soak cannot see calls your filter subtracts, or a call that fires less often than the soak runs. Headless Chrome hit both.'
date: 2026-10-20
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['deep-dive', 'security', 'systemd', 'headless-chrome', 'linux']
draft: false
product: '2.0'
---

With 11 extra syscalls admitted, headless Chrome passed every check we ran under an enforcing systemd syscall filter: start, render, restart, no kill. Then, in pre-release testing of mod_pagespeed 2.1.0, the kernel killed the browser mid-render on `mincore`, a call Chrome made in bursts every few minutes in our runs; none of our log-only soak windows had lasted a minute.

A call that fires every few minutes is invisible to a soak shorter than its period. That is one of two blind spots in "log first, then enforce what you saw". The other is any call your enforcing filter subtracts from the group the log treats as normal. Both apply to anyone running headless Chrome as a service: Puppeteer and Playwright workers, PDF renderers, link-preview fetchers.

Our case is the mod_pagespeed optimizer worker, which can render pages in headless Chrome for analysis ([measuring LCP and CLS in a headless render](/blog/headless-lcp-cls-measurement/)). Since 2.1.0 its packaged unit enforces a syscall allow-list by default ([2.1.0 release notes](/docs/release-notes-2-1/)), and a vendor drop-in re-admits what Chrome's sandbox needs ([browser analysis](/docs/browser-analysis/)). Everything below about Chrome's calls is our own observation on x86_64, sandbox on, unprivileged user: Debian 12 with Debian's chromium package, and Ubuntu 24.04 with Google Chrome's .deb, both at versions 150 and 151. The group memberships we re-checked on 19 October 2026 with `systemd-analyze syscall-filter` on systemd 252 (Debian 12) and 255 (Ubuntu 24.04).

## The profile: one group, minus two

```ini
[Service]
SystemCallFilter=@system-service
SystemCallFilter=~@privileged @resources
```

`@system-service` is systemd's set for an ordinary service. The first line makes it an allow-list, so any call outside it is killed. The `~` line removes the privileged calls and the ones that change limits and scheduling. A later plain line adds names back ([systemd.exec(5)](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html#SystemCallFilter=)).

Before 2.1.0 shipped, in pre-release testing, the unit ran log-only instead, with `SystemCallLog=~@system-service`: record any call outside the group and let it proceed. Under that soak Chrome logged four names and zero `mincore` calls.

## What Chrome needed: 12 names

| Name | Where systemd puts it | How we found it |
| --- | --- | --- |
| `seccomp`, `chroot`, `pkey_alloc`, `landlock_create_ruleset` | outside `@system-service` | soak log |
| `capset`, `setuid`, `setresuid`, `setgroups` | inside, and in `@privileged` | read off the group lists |
| `setpriority` | inside, and in `@resources` | SIGSYS at startup |
| `sched_setaffinity` | inside, and in `@resources` | SIGSYS of a renderer mid-page |
| `sched_setattr` | inside, and in `@resources` | SIGSYS at startup, version 151 only |
| `mincore` | outside `@system-service` | SIGSYS mid-render |

As a drop-in for your own unit:

```ini
[Service]
RestrictNamespaces=
SystemCallFilter=seccomp chroot capset setuid setresuid setgroups
SystemCallFilter=setpriority sched_setaffinity sched_setattr pkey_alloc landlock_create_ruleset mincore
```

The empty `RestrictNamespaces=` matters. `unshare` is inside `@system-service`, so the syscall filter does not decide whether Chrome's namespace sandbox starts. Of the unit's settings, `RestrictNamespaces=` does; on Ubuntu 24.04, AppArmor's restriction on unprivileged user namespaces is a second gate outside the unit ([24.04 release notes](https://discourse.ubuntu.com/t/noble-numbat-release-notes/39890)). We left out `setreuid` (version 150 did not call it) and `ptrace` (only Chrome's crash handler tries it, after a death). Your Node or Python driver makes calls of its own; measure those the same way.

## Blind spot one: the log trusts what the filter subtracts

`@resources` is itself a member of `@system-service` on systemd 252 and 255. The log line records calls outside `@system-service`. The `~` line removes calls inside it. A call in that gap, such as `setpriority`, is never logged however long the soak runs, and the enforcing profile kills Chrome on it at startup. Seven of the 12 names sit there. We read the four `@privileged` ones off the group lists before enforcing. The three `@resources` ones we found by kills in pre-release testing.

A non-inverted `SystemCallLog=` logs only the names it lists, and repeated lines merge ([systemd.exec(5)](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html#SystemCallLog=)). So the soak can log the deny set as well:

```ini
[Service]
SystemCallLog=~@system-service
SystemCallLog=@privileged @resources
```

We have not run this soak against Chrome; it is our reading of systemd.exec(5). We did check the merge on 19 October 2026 on systemd 252 with one command, `nice -n 5 true`, which calls `setpriority`. Under the enforcing pair it died with `status=31/SYS`. Under the first log line alone it ran unrecorded; with both, it ran and was recorded. Before you trust an empty log, run `systemctl show -p SystemCallLog <unit>` and confirm the subtracted names are gone from the exempt list.

## Blind spot two: a period longer than the soak

`mincore` is outside `@system-service`, so the log-only unit could record it, but only if a call landed in the window. One place Chromium calls it is [`ProcessMemoryDump::CountResidentBytes`](https://github.com/chromium/chromium/blob/main/base/trace_event/process_memory_dump.cc), which counts resident pages for its memory dumps; we did not trace which caller fired in our runs. We saw bursts 3 to 4 minutes apart.

The first burst that landed in an enforcing test run killed the whole browser process, not one thread, and the render in flight with it. 2.1.0 shipped with `mincore` admitted. Admitting it gives little away: since Linux 5.2, `mincore` reveals page-cache residency only for file mappings the caller owns or could open for writing; for anonymous memory it reports only the caller's own pages ([kernel commit](https://github.com/torvalds/linux/commit/134fca9063ad4851de767d1768180e5dede9a881)).

We read the bursts as timer-driven, not per page; we have not confirmed it. If that holds, more renders would not have caught it. Coverage of call sites and length in time are different axes.

## The same call, one layer down

In 2.2.0 the optimizer's cache library started calling `mincore` before it issues a readahead hint ([2.2 release notes](/docs/release-notes-2-2/)). In our tests that included the cache reopen at start. The worker's own allow-list did not have the name. A default install hides that, because the browser drop-in ships active and admits `mincore`. A pre-release test with that drop-in masked caught the worker being killed on cache reopen, and 2.2.0 ships the cache change and `SystemCallFilter=@system-service mincore` together. Every kill in this post was seen in pre-release testing, never in a shipped release.

The filter is per unit, so one drop-in can hide a gap in another. Test each layer with the others masked.

## How to soak a long-lived service for periodic calls

**Know what the log shows.** `SystemCallLog=` records the calls in its set that the process made while the soak ran, and lets them proceed. It says nothing about calls outside its set, calls not made during the window, or code paths you did not exercise.

**Size the soak from the slowest timer, not the request rate.** A call made every N minutes is invisible to any soak shorter than N. Run for at least two periods of the slowest periodic thread you know of, and leave part of it idle: background timers fire on the wall clock, often while the process waits for work. If you do not know a component's periods, read its source for timers or soak for hours.

**Re-run on every upgrade of a component you don't control.** Version 151 added `sched_setattr` one day after we had measured 150.

## Reading a SIGSYS kill

We kill rather than return an errno, so a missing name fails at its cause instead of somewhere downstream. When auditing is enabled, the kernel writes a type 1326 record for a logged or killed call. This one is from that check:

```text
type=1326 ... comm="nice" exe="/usr/bin/nice" sig=31 arch=c00000b7 syscall=140 compat=0 ip=... code=0x80000000
```

`sig=31` with `code=0x80000000` is a process kill; `sig=0` with `code=0x7ffc0000` is a log-only entry. `arch` names the ABI (`c000003e` is x86_64, `c00000b7` is aarch64), and `syscall` is a number in that ABI's table. `ausyscall aarch64 140` prints `setpriority`; on x86_64, 27 is `mincore` ([syscall table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)).

```sh
ausearch -m SECCOMP -ts recent -i   # auditd running; -i names the call
journalctl -k | grep type=1326      # auditd not running
```

The kernel copies audit records into its own log only while auditd is not running. Where auditd is running (`systemctl is-active auditd`), an empty `journalctl -k` proves nothing; use `ausearch`. `systemctl status` shows `status=31/SYS` only when the unit's main process died. When the victim is a Chrome child, the unit stays up. Besides the audit record, the only trace is in your driver: a browser-disconnect error if the browser process died, or a crashed page or target if a renderer did. Log the exit signal and alert on both.

## Takeaway

A clean log-only soak proves that the calls it saw are inside the set it logged against, for the window it ran and the versions you ran. Log the groups you subtract, idle for longer than the slowest timer you know of, test each drop-in with the others masked, and page on SIGSYS.
