# Enforcing-SELinux EL9 upgrade rehearsal (Hyper-V VM)

The 1.15 → 1.16 in-place upgrade rehearsal, run on a **real, SELinux-enforcing
AlmaLinux 9 VM** instead of a container. A tracked gap in the container
rehearsal motivates this: a booted-systemd container shares the host kernel
and can never be enforcing, so `install/upgrade_test/run_upgrade_test.sh`
proves the upgrade path but not what the stock targeted policy does when
`httpd_t` meets the optimizer daemon's memory-mapped cache volume and unix
notify socket. The daemon's own policy module
(`deploy/selinux/pagespeed-optimizer.{te,fc}` in the daemon repo) is a draft
that no package installs. One enforcing-EL9 VM run of this rehearsal is a GA
gate for 2.1: *no AVC denials on `httpd_t` against the cache volume or the
notify socket, and in-place optimization demonstrably on, with SELinux
enforcing.*

The VM runs under Hyper-V on the build host, following the same pattern as
the IIS rig (`test/system/iis/ci-msi-upgrade-test.ps1`) and the cPanel rig
(`test/system/cpanel/`), sitting next to their VMs.

## Architecture

```
            Hyper-V host (Windows, elevated PowerShell 5.1)
            ┌──────────────────────────────────────────────────────────────┐
            │  provision-el9-vm.ps1        (once)                         │
            │    qcow2 -> VHDX (qemu-img, PATH or WSL) -> Resize-VHD     │
            │    cloud-init NoCloud seed ISO (IMAPI2, label cidata)        │
            │    New-VM Gen2 -> first boot -> dnf update + tooling         │
            │    -> poweroff -> Checkpoint-VM 00-fresh                     │
            │                                                              │
            │  run-el9-selinux-rehearsal.ps1   (per run)                   │
            │    Restore-VMSnapshot 00-fresh ─┐                            │
            │    Start-VM                     │                            │
            │    ARP-by-MAC ──────────────────┼──► guest IP (TCP/22 live)  │
            │    scp payload ─────────────────┘   driver + Puzzle.jpg      │
            │    ssh root: el9-selinux-rehearsal.sh    + rpms [+ .te/.fc]  │
            │    scp artifacts back; Stop-VM; Restore-VMSnapshot           │
            │                                                              │
            │   ┌──── Hyper-V "Default Switch" (NAT) ─────────────────┐    │
            │   │  EL9-SELinux-rehearsal  ──►  checkpoint 00-fresh    │    │
            │   │  AlmaLinux 9 GenericCloud, SELinux ENFORCING        │    │
            │   └─────────────────────────────────────────────────────┘    │
            └──────────────────────────────────────────────────────────────┘
```

## Files

| File | Runs where | Purpose |
|---|---|---|
| `provision-el9-vm.ps1` | host | One-time: image → VHDX, seed ISO, Gen-2 VM, first boot, `00-fresh` checkpoint. `-DryRun`, `-BuildSeedOnly`. |
| `run-el9-selinux-rehearsal.ps1` | host | Per run: restore → start → stage → run driver → collect artifacts → restore. `-DryRun`. |
| `hv-common.ps1` | host | Shared helpers (ssh/scp via `Start-Process`, ARP-by-MAC IP discovery, IMAPI2 seed ISO, qemu-img routing). Dot-sourced. |
| `el9-selinux-rehearsal.sh` | guest (root) | The rehearsal itself: baseline 1.15 → upgrade to the 1.16 pair → assertions, with SELinux enforcing; AVC accounting per phase; diagnostics. `--dry-run`. |
| `cloud-init/user-data.template`, `meta-data.template` | host → seed | NoCloud seed: hostname, key-only root SSH. **No SELinux lines** — the image default (enforcing) is the point. |

## Prerequisites

- The Hyper-V host: Hyper-V role with the `Default Switch` (NAT), an elevated
  Windows PowerShell 5.1, the Windows OpenSSH client (`ssh.exe`/`scp.exe`).
  The same host that carries the IIS and cPanel test VMs is the intended one;
  nothing here assumes a machine name.
- **Idle host.** One Hyper-V test VM at a time. Before provisioning or running,
  confirm no release job is using the IIS/cPanel VMs (`Get-VM` shows them
  `Off`/`Saved`; the repo's Actions runners on that host show `busy=false`).
- An SSH keypair the host can use unattended: default
  `%USERPROFILE%\.ssh\id_ed25519` + `.pub` (`-SshKeyPath` /
  `-SshPublicKeyPath`). The public key goes into the seed; the private key is
  what the runner uses. If a runner *service* is ever to run this, the service
  account's profile needs the key.
- `qemu-img` for the one-time qcow2 → VHDX conversion: either inside WSL
  (`sudo apt-get install -y qemu-utils`; the script finds it there) or on
  Windows (`winget install SoftwareFreedomConservancy.QEMU`). Without it,
  convert by hand and pass `-ImageVhdx`.
- The image: AlmaLinux 9 GenericCloud, x86_64
  (`https://repo.almalinux.org/almalinux/9/cloud/x86_64/images/AlmaLinux-9-GenericCloud-latest.x86_64.qcow2`,
  verified against the `CHECKSUM` file next to it). `-DownloadImage` does the
  fetch + sha256 check; or point `-ImageQcow2` at a copy already on disk.
- `gh` (authenticated for the release repo) **only** when the runner should
  download the 1.16 pair from a GitHub release itself. With `-PackagesDir`
  (rc.14 candidates, or locally built rpms) or `-RepoUrl` it is not needed.
- On the guest, nothing to prepare: the driver installs what it needs.

## One-time: provision the VM

```powershell
cd <repo>\test\system\el9-selinux
.\provision-el9-vm.ps1 -DryRun -ImageQcow2 E:\path\AlmaLinux-9-GenericCloud-latest.x86_64.qcow2   # plan only
.\provision-el9-vm.ps1 -ImageQcow2 E:\path\AlmaLinux-9-GenericCloud-latest.x86_64.qcow2
# or:  .\provision-el9-vm.ps1 -DownloadImage
# or:  .\provision-el9-vm.ps1 -ImageVhdx <already converted>.vhdx
```

What it makes: VM `EL9-SELinux-rehearsal` (Gen 2, Secure Boot **Off** — pass
`-SecureBoot On` to use the `MicrosoftUEFICertificateAuthority` template
instead; 2 vCPU, 4 GB static, 32 GB dynamic VHDX under the host's
`VirtualHardDiskPath`, seed ISO under `<VirtualMachinePath>\seeds\`),
booted once over the Default Switch, `cloud-init status --wait`, **asserts
`getenforce` = `Enforcing`** (refuses otherwise), `dnf -y update` (skip:
`-NoUpdate`), installs `httpd` + the SELinux tooling every run needs anyway
(`audit`, `policycoreutils-python-utils`, `setools-console`), powers off, and
takes checkpoint **`00-fresh`**. The VM is left **Off** at that checkpoint.
`00-fresh` therefore means: *AlmaLinux 9 + updates + the web server, nothing
of ours, SELinux enforcing* — the same "distro + systemd + web server" state
the container rehearsal builds its image to.

The seed ISO is built with `genisoimage` inside WSL when that exists (the
tool the host's existing EL seeds were made with: Joliet + Rock Ridge, exact
names) and otherwise with Windows' built-in IMAPI2 (`-SeedTool auto|wsl|imapi2`).
Both variants were checked by loop-mounting them in Linux: each presents
`meta-data` and `user-data` at the root (IMAPI2 stores Joliet names with a
`;1` suffix, which isofs strips).

`-BuildSeedOnly` renders the templates and builds the seed ISO into
`-WorkDir` without touching Hyper-V (a way to check the seed, or to build it
for a VM created by hand). Re-baking `00-fresh` later (newer packages):
`Remove-VMSnapshot -VMName EL9-SELinux-rehearsal -Name 00-fresh`, boot, update,
power off, `Checkpoint-VM ... -SnapshotName 00-fresh` — or remove the VM +
VHDX and re-provision (the script refuses to overwrite either).

## Per run: one command

```powershell
cd <repo>\test\system\el9-selinux

# rc.14 candidates (or locally built rpms): exactly one optimizer + one module rpm in the dir
.\run-el9-selinux-rehearsal.ps1 -PackagesDir D:\pkgs\rc14

# the pair from the GitHub release v1.16.0-rc.14 (gh, sha256-verified against SHA256SUMS)
.\run-el9-selinux-rehearsal.ps1 -Rc 1.16.0-rc.14

# the staging-repository path (credentials from PACKAGES_USER / PACKAGES_PASS in the environment)
.\run-el9-selinux-rehearsal.ps1 -Rc 1.16.0-rc.14 -RepoUrl https://<staging endpoint>/staging

# measure the daemon repo's DRAFT policy module instead of its absence
.\run-el9-selinux-rehearsal.ps1 -PackagesDir D:\pkgs\rc14 -Policy draft -PolicySrc <daemon repo>\deploy\selinux

# stage + print the exact guest command, no Hyper-V, no ssh
.\run-el9-selinux-rehearsal.ps1 -PackagesDir D:\pkgs\rc14 -DryRun
```

A run restores `00-fresh`, boots, stages `/tmp/el9-rehearsal/` on the guest
(driver, `Puzzle.jpg`, `pkgs/`, `selinux/`), runs the driver as root, copies
`/tmp/el9-rehearsal/artifacts/` back to `<WorkDir>\artifacts\<timestamp>\`
(next to `guest-run.log` and `runner-transcript.log`), stops the VM and
restores `00-fresh` again. Budget ~10–15 minutes; the guest driver's exit
code is the runner's. `-KeepRunning` leaves the VM up for a hands-on look
(`ssh root@<ip>`; the IP is in the log).

**Against rc.14 packages specifically:** stage the two rpms
(`pagespeed-optimizer-1.16.0~rc.14-*.x86_64.rpm`,
`mod-pagespeed-1.16.0~rc.14-*.x86_64.rpm`, GitHub spells `~` as `.`) in a
directory and pass `-PackagesDir`. This is the same source the container
rehearsal's `--packages-dir` takes; with it the packaged daemon
drop-in `pagespeed_daemon.conf` is asserted **strictly** (owned by the
package, `%config`, read after the module loader in `httpd -t -D
DUMP_INCLUDES`), exactly as the container rehearsal does it. For an older
pair the driver writes the two directives by hand and says so.

### The guest driver on its own

`el9-selinux-rehearsal.sh` is what the runner executes; it can be run by
hand on any enforcing EL9 host as root (`--help` for the flags,
`--dry-run` prints the resolved plan and exits before touching anything —
also works off-host). Phases, mirroring the container rehearsal:

0. preflight — EL9, **`getenforce` must be `Enforcing`** (exit 2 otherwise),
   auditd up, tooling; records the audit-log position.
1. fixture — `/var/www/html/upgrade-fixture/` with an image, a stylesheet, a
   page; `restorecon` so it is `httpd_sys_content_t`.
2. baseline — `install.sh` + `dnf install mod-pagespeed` (1.15 from the
   public repository), a customized conffile, the web server enabled;
   proves 1.15 optimizes (version header, rewritten URL, smaller image).
   `--baseline-selinux rw-content` (default) first labels the module's own
   cache and log directories web-server-writable (see *Findings* below for
   why); `none` keeps default labels. AVCs during this phase are reported
   as a **separate finding** (`WARN`), not the gate.
3. upgrade — `dnf install` of the staged pair, or `install.sh` + `dnf
   upgrade` from `--repo-url`; version assertions; the drop-in assertion;
   optionally `--policy draft` (build with `selinux-policy-devel`,
   `semodule -i`, `restorecon` of the daemon's paths, daemon restart, assert
   it runs as `pagespeed_t`); management API socket on; web-server restart.
4. assertions — daemon active as `pagespeed`, web-server child carries the
   `pagespeed` gid **and still runs as `httpd_t`**, conffile untouched,
   module loaded, version header, zero daemon-startup log signatures, the
   daemon's
   `notifications.received` counter moves (the module reaches the socket),
   a URL 1.15 never saw is served smaller (in-place optimization), SELinux
   still enforcing, and **zero AVC/USER_AVC denials since the upgrade began
   for `comm=httpd` and for `comm=pagespeed-optimizer`**.
5. diagnostics into `--artifacts`: `selinux-status.txt`, `avc-since-*.txt`
   (`ausearch -i`), `audit2allow*.te` (the rules a policy would need),
   `sealert.txt` (if `setroubleshoot-server` is installed), `semodule
   -pagespeed.txt`, `daemon-journal.log`, `httpd-dump-includes.txt`,
   `httpd-modules.txt`, `contexts.txt` (labels of every path, domains of
   every process, fcontext rules, httpd booleans), `error-log-*.log`,
   `daemon-stats.json`, package logs. `--disable-dontaudit` (`semodule -DB`
   for the run) surfaces denials the policy hides.

## What PASS means for the GA gate

All of the following, in one run, with `getenforce` = `Enforcing` at start
and end:

| Check | Meaning |
|---|---|
| `AVC/USER_AVC denials for comm=httpd since the upgrade: 0` | `httpd_t` was never denied the cache volume (`{ read write open map }` on the volume file, `search` on the dir) or the notify socket (`write` on the notify sock_file, `connectto` on the daemon's stream socket). One measured stock class is excluded and reported separately: the root httpd parent's `SO_SNDBUFFORCE` `net_admin` denial (rehearsal §4.3: not pagespeed's, dontaudited by the distro on purpose, surfaced only because `--disable-dontaudit` disables dontaudit for the run; the shipped policy carries no allow for it — see `avc_stock_net_admin` in the driver) |
| `... for comm=pagespeed-optimizer since the upgrade: 0` | the daemon's own domain was never denied (relevant with a policy that confines it) |
| `daemon notification counter moved` | the module really reached the socket |
| `post-upgrade in-place optimization: Puzzle-after-upgrade.jpg served at N bytes` | in-place optimization works through the daemon |
| `web-server child still runs confined as httpd_t` | the run measured the policy, not an unconfined web server |

The summary line reads
`=== el9-selinux (<policy> policy, baseline-selinux <mode>): P passed, F failed, W warning(s) -- 1.15.x -> 1.16.0~rc.N + optimizer ...`
followed by `SELinux: start=Enforcing end=Enforcing; AVCs since upgrade:
httpd=0 daemon=0 all=…; stock net_admin excluded=N (pre-policy A,
post-policy B)`. `F = 0` is PASS.

### What to expect today

- **`-Policy none` (what the packages ship): PASS is NOT expected.** No
  package installs any policy, so the daemon runs as
  `unconfined_service_t`, its cache volume and socket carry the generic
  `var_t` / `var_run_t` labels, and the stock policy gives `httpd_t` none of
  `{ write map }` on `var_t` files, `write` on a `var_run_t` sock_file, or
  `connectto` to an `unconfined_service_t` stream socket. The expected
  report is the known daemon-startup failure, *with a misleading group hint*
  in the error
  log (DAC is fine; the AVC is the cause), `httpd` denials > 0, notification
  counter unchanged, image not optimized. That FAIL is the gate doing its
  job: it turns "untested" into a measured list of denials, and
  `artifacts/audit2allow-since-upgrade.te` is the exact rule set a policy
  must grant.
- **`-Policy draft`:** the draft grants `httpd_t` exactly those permissions
  on `pagespeed_cache_t` / `pagespeed_runtime_t` / `pagespeed_t`, so the
  `httpd` denials should go to zero **if** the labels take (the driver
  asserts the daemon runs as `pagespeed_t` after `restorecon` + restart).
  The daemon's own domain is where the draft is thin: `pagespeed_t` only
  gets its cache and runtime directories, so expect denials for the daemon
  itself (e.g. `/tmp` under `PrivateTmp`, `/proc` reads, executing `curl`
  for the agent_optimize fetcher, journald/stdout details). The .te's own
  header says as much ("an unvalidated policy on an enforcing host breaks
  the daemon"). Those `pagespeed-optimizer` denials fail the gate too, on
  purpose — a policy that confines the daemon has to let it work — and the
  `audit2allow` output is the list of rules to add.
- **Baseline (1.15) finding, both modes:** with default labels
  (`--baseline-selinux none`) 1.15's own cache at `/var/cache/mod_pagespeed`
  is `var_t`, which `httpd_t` cannot write or map, so 1.15 does not optimize
  on a stock enforcing EL9 host either. The documented fix in the 1.1
  troubleshooting page (`chcon -R -t httpd_sys_content_t /var/cache/pagespeed/`)
  is wrong on three counts for the EL rpm: the path (`/var/cache/mod_pagespeed`
  and `/var/log/pagespeed`), the type (`httpd_sys_content_t` is read-only for
  `httpd_t` unless `httpd_unified` is on — `httpd_sys_rw_content_t` is the
  writable one; `httpd_log_t` for the log dir), and the tool (`chcon` does not
  survive a relabel; `semanage fcontext -a` + `restorecon` does). The default
  `rw-content` mode applies the corrected recipe so the 1.16 daemon-path
  denials are measured on their own. **Follow-up:** fix that page (daemon
  repo, `website/src/content/docs-1.1/troubleshooting.md`) and consider
  shipping the fcontext rules for the module's own directories in the module
  rpm (`%post semanage`/`restorecon`, or the same policy module).

## On denials: the decision the gate must produce

Two ways to make `-Policy none` pass on a stock enforcing EL9 host, and only
one of them is complete:

**A. Package the policy module (recommended).** Finish the draft
(`pagespeed-optimizer.te/.fc`) against the `audit2allow` output of a
`-Policy draft` run until both denial counters are zero, then ship it: build
the `.pp` at rpm build time (`selinux-policy-devel`,
`make -f /usr/share/selinux/devel/Makefile`), install it from the
**optimizer** rpm's scriptlets (it owns the types; the `httpd_t`/`nginx_t`
peer rules live in the same module) with the standard `%selinux_modules_install`
/ `%selinux_modules_uninstall` / `%selinux_relabel_post` macros
(`Requires(post): selinux-policy-targeted policycoreutils`), `restorecon` the
daemon's paths on install, and add an enforcing EL9 leg to the daemon's
package rig (its `selinux` leg already reports SKIP with this exact blocker).
The module packages need nothing extra: `httpd_t`'s rules arrive with the
policy. Debian/Ubuntu are unaffected (AppArmor; no `httpd_t`).

**B. Document a boolean / manual recipe.** There is **no stock boolean** that
lets `httpd_t` `connectto` an `unconfined_service_t` socket or map a `var_t`
file. Labelling the volume `httpd_sys_rw_content_t` by hand covers the
mmap-rw half but leaves the socket `connectto` denied — so B cannot be made
complete without a module. The only *documented-only* escape hatches are
`semanage permissive -a httpd_t` (the web-server domain stops being
confined; unacceptable as the product's default answer, useful as a
**diagnostic** to see every denial in one run) and `setenforce 0`. B is at
best an interim note for the release notes ("on enforcing EL9, in-place
optimization through the daemon needs the policy module; until it ships,
…") — not a GA-quality answer.

The rehearsal produces the evidence for the GA gate; it does not decide it.
Attach `guest-run.log`, `avc-since-upgrade.txt` and
`audit2allow-since-upgrade.te` from the artifacts to the gate record.

## Troubleshooting

- **No guest IP.** `Get-GuestIp` reads the host's neighbor table for the
  VM's MAC on `*Default Switch*` and only returns an address that answers on
  TCP/22. After a host reboot the Default Switch re-randomizes its subnet and
  stale `Permanent` entries linger; the helper tries `Reachable`/`Stale`
  first for that reason. If it still times out: the guest did not boot
  (Secure Boot? try `-SecureBoot Off`, the default), cloud-init failed (open
  the console in Hyper-V Manager), or sshd is not up yet.
- **`SSH never came up`.** Key mismatch: the seed carries the `.pub` given at
  provisioning; the runner must use the matching private key
  (`-SshKeyPath`). A runner *service* runs as another account — give it the
  key or pass the path.
- **`cloud-init status` exit 2** is "recoverable errors"; the driver prints
  the status. Look at `/var/log/cloud-init.log` on the guest.
- **`guest is not SELinux-enforcing after first boot`.** Wrong image (a
  hardened/desktop variant) or a seed that changed the mode; the provisioning
  script refuses to checkpoint such a VM.
- **`qemu-img is not available`.** Install `qemu-utils` in WSL or QEMU on
  Windows, or convert elsewhere and pass `-ImageVhdx`.
- **Driver exit 2 (`SELinux is 'Permissive'`)** on a hand-run: this rig
  measures enforcing; set `SELINUX=enforcing` in `/etc/selinux/config` and
  reboot.
- **Left-over VM state.** The runner restores `00-fresh` in its trap; if the
  host itself died mid-run: `Stop-VM -Name EL9-SELinux-rehearsal -TurnOff -Force;
  Restore-VMSnapshot -VMName EL9-SELinux-rehearsal -Name 00-fresh -Confirm:$false`.

## CI wiring (not done here)

Not wired into `release.yml` yet: the gate is a one-off owner-ruled run, and
its expected result today is FAIL until a policy ships. When it becomes a
recurring leg, mirror `ea4-cpanel-smoke`: pin to the heavy-host label via
`vars.CI_HEAVY_LABEL`, an `hv-check` step that skips cleanly when the VM or
`00-fresh` is missing, stage the rc's rpm pair from the release archive into
a CI-private directory, call `run-el9-selinux-rehearsal.ps1 -PackagesDir …`,
upload `<WorkDir>\artifacts\<stamp>` on failure.
