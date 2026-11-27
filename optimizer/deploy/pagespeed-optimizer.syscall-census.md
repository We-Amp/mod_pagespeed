# Optimizer daemon syscall census

Produced by `tools/packaging/syscall-census.sh`, which straces the daemon
inside the same rig images the package-install rig uses and intersects the
result with each distro's own systemd syscall groups. Regenerate with:

    tools/packaging/syscall-census.sh --distro debian12 \
        --binary <factory_worker> --library <libpagespeed.so>

This is the MEASURE half of "measure, then filter". It is a manual/periodic
lane, not a per-PR gate: its output is an input to a decision (which systemd
groups the profile names), and the per-PR assertions check the consequence of
that decision instead -- see `tools/packaging/verify-daemon-systemd.sh`.

**Measured 2026-08-29**, `x86_64`, against `factory_worker` built from
`15861931` -- the commit this branch was cut from, pinned deliberately
because "main" moves and a census that names a moving target cannot be
reproduced.

## Verdict

**Empty on all three distros.** Every syscall the daemon makes, in every
measured posture, is already admitted by

    SystemCallFilter=@system-service
    SystemCallFilter=~@privileged @resources

so the enforcing profile carries no per-name additions from the census.
(The unit since admits one name by hand, `mincore`, for the cache library's
readahead residency check added after this census; see the comment above
`SystemCallFilter=` in the unit.) That is the result
that lets the profile ship as two group names instead of a list of ~370
syscalls that would have to be re-derived per architecture.

**Shipped posture (2.1.0):** those two lines are in the packaged unit itself
(`deploy/pagespeed-optimizer.service`), enforcing by default, together with
`RestrictNamespaces=yes`; the unit also keeps `SystemCallLog=~@system-service`
so calls outside the group stay recorded. Until 2.1.0 the same two lines shipped
as an opt-in drop-in example and the unit was log-only. The flip was decided
on this census plus the rig's enforcing legs; the first extended soak of the
enforcing posture is the release-candidate soak that precedes GA; the documented opt-out
(`90-syscall-filter-off.conf.example`) resets both directives from `/etc`,
where no package upgrade can reach it. The browser-analysis re-admissions
ship installed and active as a separate vendor drop-in
(`20-browser-analysis.conf`, next to the unit) rather than folded into the
unit, so that layer can be turned back into an opt-in on its own if the
browser path ever shows a kill; a host without browser analysis masks it with
an empty same-named file under `/etc`.

A note on how the census itself is taken now that the unit enforces: posture
P8 wraps `ExecStart` in `strace`, which needs `ptrace(2)` -- outside
`@system-service` -- so the census drop-in resets `SystemCallFilter=` for the
traced run (and sorts last, `zz-`, so the reset wins). Every other directive
stays in force, and `SystemCallLog=` is untouched. What P8 measures is "every
call the daemon makes under this unit", which is the input the allow-list is
decided from; whether the allow-list then admits them is what the rig's
`h6-*` legs assert, on every package build.

| Distro | systemd | @system-service | minus @privileged+@resources | observed | would be killed |
| --- | ---: | ---: | ---: | ---: | ---: |
| Debian 12 | 252 | 376 | 345 | 70 | **0** |
| Ubuntu 24.04 | 255 | 375 | 344 | 69 | **0** |
| AlmaLinux 9 | 252 | 370 | 340 | 72 | **0** |

## What was measured, and what was not

| # | Posture | Measured |
| --- | --- | --- |
| P1 | cold start, cache create + volume open, three UDS binds, peer library load, SIGTERM | yes |
| P2 | P1 + `--api-socket`, **with real requests driven over it** | yes |
| P3 | P1 + `--api-port` + token, with real requests | yes |
| P7 | P1 + a reload signal before shutdown | yes |
| **P8** | **the daemon under its own shipped unit** -- `User=pagespeed`, every shipped directive applied by systemd, strace wrapped around `ExecStart` so startup is traced too, then health/stats/config/cache-urls/purge driven over the API socket | **yes** |
| P5/P6 | the `curl` child the agent_optimize fetcher spawns | measured standalone |
| P4 | browser analysis + Chrome | **measured separately** -- by the rig's `h6-browser-analysis` leg under the real unit, not by strace (see below) |

P8 matters more than its row suggests. P1-P3 and P7 run the binary directly,
as root, outside systemd -- which is the wrong process for a profile that
systemd installs. Under the unit the daemon is unprivileged with
`ProtectSystem=strict`, `PrivateTmp`, `ProtectProc` and the address-family
restriction all in force, and that changes which calls glibc and libuv
actually take. P8 is also what first exercised a *served connection*: an
earlier revision of this census bound the API socket in P2/P3 but never
connected to it, so `accept4` appeared in no posture at all and the whole
serving half of the socket path was silently missing from the artefact. It is
present now, on all three distros.

One honest gap remains, and one former gap is now closed:

* **P4 (browser analysis with a real Chrome) is measured, differently.** An
  earlier attempt in a purpose-built Chromium container was abandoned because
  Chromium crashed there regardless of profile. It now runs: Chromium 150 from
  the distro package, in the rig's Ubuntu 24.04 and Debian 12 images, under the
  real unit as user `pagespeed` with `--browser-sandbox=require`. Under the
  SHIPPED (log-only) unit Chrome starts sandboxed, renders, and the audit log
  records four names outside `@system-service` *within that window*: `seccomp`,
  `chroot`, `pkey_alloc`, `landlock_create_ruleset`. Under the ENFORCING drop-in with the
  original six-name browser drop-in, Chrome was killed with SIGSYS on
  `setpriority` -- a member of `@resources`, which the enforcing file subtracts
  and which the log-only soak therefore **cannot** record, because it is inside
  `@system-service`. Re-admitting it exposed `sched_setaffinity` (same group)
  and the two logged names above; Chromium 151 (Debian 12's package the day
  after) then added `sched_setattr`, a third `@resources` member. With all five
  added, both versions survive startup, repeated renders and a service restart
  with no kill. A sixth name arrived later and exposed a different blind spot:
  `mincore`, called by Chromium's MemoryInfra thread in a burst every few
  minutes. It is outside `@system-service`, so the log-only soak
  **can** record it -- but only if a burst lands inside the observation
  window, and every sub-minute render cycle recorded zero calls. The first
  burst to fire under the enforcing profile (1.16.0-rc.11 CI, mid-render)
  killed the whole browser process; `mincore` is now re-admitted alongside
  the other five. A periodic call is invisible to any soak shorter than its
  period, however long the soak's *coverage* of call sites is. The browser
  drop-in now
  carries those names and is no longer EXPERIMENTAL; the rig's
  `h6-browser-analysis` leg re-measures it on every package build (SKIP, with
  the reason, on the EL9 image, which has no Chromium in its base repos).
  A window long enough to *catch* the next minutes-scale periodic call is a
  periodic cadence, not a per-PR one, so that half runs weekly: the rig's
  `h6-browser-soak` leg (#1485) idles a sandboxed Chrome under the enforcing
  profile for two burst periods and asserts zero kills (CI Periodic's
  `daemon-rig-soak-x64` job; a named SKIP in the per-PR rig matrix).
  Not re-admitted: `ptrace`, attempted only by `chrome_crashpad_handler` after a
  Chrome process has already died -- the kill costs a crash report, not the
  browser.
* **A fully served optimization is not measured.** The daemon optimizes over
  the notify socket on behalf of the serving module; the management API has no
  optimize endpoint (`/v1/health`, `/v1/stats`, `/v1/config`, `/v1/metrics`,
  `/v1/cache/*` -- no optimize). Driving one end to end needs
  nginx plus the module, which is a different CI lane. P8's cache purge and
  reprocess calls exercise the cache write path; the image codecs are
  in-process and statically linked, so they add no syscalls beyond the
  mmap/futex/madvise family P1 already covers.

Only `x86_64` was measured. The aarch64 question is answered by assertion
rather than census: the rig runs the full `h6-*` leg set on aarch64, where a
call outside the profile would surface as a SIGSYS kill.

## The questions the design asked, answered from measurement

**io_uring (design §7 R1 -- the #1 measured unknown).** libuv 1.52's unified
Linux backend really does take the io_uring path here: `io_uring_setup` and
`io_uring_enter` appear in the census on all three distros. Both are members
of `@system-service`, so the profile needs no io_uring carve-out and the
daemon needs no `UV_USE_IO_URING=0` -- which matters, because that env
opt-out would have been a silent performance change on an operator's machine.

**`unshare` is inside `@system-service`.** So the syscall allow-list is NOT
what governs the headless-Chrome sandbox probe; `RestrictNamespaces=` is.
That is why the enforcing unit carries `RestrictNamespaces=yes` and the
browser-analysis drop-in resets it, and why the probe under the enforcing
profile reports the namespace denial rather than a SIGSYS kill.

**The browser drop-in's re-admissions are necessary, and measured.**
`seccomp` and `chroot` are not in `@system-service`; `capset`, `setuid`,
`setresuid` and `setgroups` are in it but *also* in `@privileged`, which the
enforcing unit subtracts. Chrome's layer-1 sandbox drops capabilities and
chroots, and its layer-2 filter is `seccomp(2)` -- so without those names
re-admitted the enforcing profile would kill it. Read with
`systemd-analyze syscall-filter`, not inferred from documentation.

**`@sandbox` cannot be used to say that.** The group does not exist before
systemd 254: on Debian 12 and AlmaLinux 9 (both 252) `systemd-analyze
syscall-filter @sandbox` answers `Filter set "@sandbox" not found`, and
systemd loads a unit naming it anyway, with a warning, silently dropping it
from the allow-list. The drop-in therefore uses plain syscall names, and a
rig leg fails the build if `systemd-analyze verify` emits any unknown-group
warning.

That guard needed a correction of its own before it was worth anything. The
warning goes to **stderr**, and the leg first read it through a helper that
discarded stderr -- so it saw an empty string and passed for the broken
spelling too. It now captures stderr, and it carries its own negative half:
on a systemd that does not know `@sandbox` (252, so Debian 12 and AlmaLinux
9) the leg plants a drop-in naming it and asserts the warning **is** seen,
then removes it. On systemd 254+ the group is legitimate and the control
cannot distinguish anything, so it logs an explicit SKIP rather than passing
quietly.

**`AF_INET`/`AF_INET6` cannot be dropped by default (design §6.2
corrected).** The census measures a real `curl` child standalone,
because `curl` is what the agent_optimize fetcher spawns and it inherits the
service's `RestrictAddressFamilies=`. `AF_NETLINK` is likewise load-bearing:
it is glibc's NSS/`getaddrinfo` path.

**`swapon` and `init_module` are denied on all three.** Both are outside
`@system-service`; `swapon` is what the rig's positive control calls, so the
control is denied on every measured distro -- and the rig runs it a second
time with the drop-in removed to prove it is not denied then.

Incidental finding, filed as its own issue (#1465, since fixed): the census
surfaced that the daemon installed no `SIGHUP` handler and the unit declared
no `ExecReload=`, so `systemctl reload` terminated it. P7 is what surfaced
that. The daemon now ignores `SIGHUP` by design -- there is no reload
semantics to trigger -- and the unit declares an `ExecReload=` that fails
loudly and points at `systemctl restart`, so P7's signal now leaves the
daemon running and contributes no calls to the trace.

## Observed syscalls, per distro

Differences between the three are glibc's, not the daemon's -- `readlink` vs
`readlinkat`, `fstat` vs `newfstatat`, `pipe` vs `pipe2`, `sysinfo` vs
`statfs`. This is exactly the per-architecture and per-libc variation that
makes a hand-maintained syscall NUMBER list the wrong artefact and a systemd
GROUP name the right one.

### Debian 12 (systemd 252)

    accept4 access arch_prctl bind brk chmod clock_nanosleep clone3 close
    connect epoll_create1 epoll_ctl epoll_pwait eventfd2 execve exit
    exit_group faccessat2 fchmod fcntl fsync ftruncate futex getdents64
    geteuid getpeername getpid getrandom getsockname getsockopt gettid
    io_uring_enter io_uring_setup ioctl listen lseek madvise mmap mprotect
    msync munmap newfstatat openat pipe2 poll pread64 prlimit64 pwrite64
    read readlink recvfrom recvmsg rename restart_syscall rseq rt_sigaction
    rt_sigprocmask rt_sigreturn sendmmsg sendto set_robust_list
    set_tid_address setsockopt socket socketpair sysinfo umask uname unlink
    write

Would be killed by the enforcing profile: **none**

### Ubuntu 24.04 (systemd 255)

    accept4 access arch_prctl bind brk chmod clock_nanosleep clone3 close
    connect epoll_create1 epoll_ctl epoll_pwait eventfd2 execve exit
    exit_group faccessat2 fchmod fcntl fstat fsync ftruncate futex
    getdents64 geteuid getpeername getpid getrandom getsockname getsockopt
    gettid io_uring_enter io_uring_setup ioctl listen lseek madvise mmap
    mprotect msync munmap newfstatat openat pipe2 poll pread64 prlimit64
    pwrite64 read readlinkat recvfrom recvmsg rename restart_syscall rseq
    rt_sigaction rt_sigprocmask rt_sigreturn sendmmsg sendto set_robust_list
    set_tid_address setsockopt socket umask uname unlink write

Would be killed by the enforcing profile: **none**

### AlmaLinux 9 (systemd 252)

    accept4 access arch_prctl bind brk chmod clock_nanosleep clone3 close
    connect epoll_create1 epoll_ctl epoll_pwait eventfd2 execve exit
    exit_group faccessat2 fchmod fcntl fstat fsync ftruncate futex
    getdents64 geteuid getpeername getpid getrandom getsockname getsockopt
    gettid io_uring_enter io_uring_setup ioctl listen lseek madvise mmap
    mprotect msync munmap newfstatat openat pipe pipe2 poll pread64
    prlimit64 pwrite64 read readlink recvfrom recvmsg rename restart_syscall
    rseq rt_sigaction rt_sigprocmask rt_sigreturn sendmmsg sendto
    set_robust_list set_tid_address setsockopt socket socketpair statfs
    umask uname unlink write

Would be killed by the enforcing profile: **none**
