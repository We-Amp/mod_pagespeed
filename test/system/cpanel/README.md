# EA4 / cPanel end-to-end smoke rig

Tooling for the `ea4-cpanel-smoke` job in the release lane.
Builds on the same Hyper-V-on-a-Windows-host pattern as the IIS rig
(`test/system/iis/ci-msi-upgrade-test.ps1`).

## Architecture

```
                Windows rig host (CI runner)
                ┌────────────────────────────────────────────────┐
                │   ci-ea4-smoke.ps1                             │
                │                                                │
                │   Restore-VMSnapshot ─┐                        │
                │   Start-VM            │                        │
                │   ARP-by-MAC lookup ──┼──► guest IP            │
                │   scp RPM/token/sh ───┘                        │
                │   ssh + run target script                      │
                │                                                │
                │   ┌──── Hyper-V (Default Switch / NAT) ────┐   │
                │   │   CP-EL9-base ─► 01-cpanel-clean-el9   │   │
                │   └────────────────────────────────────────┘   │
                └────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|---|---|
| `ci-ea4-smoke.ps1`         | Runner-side driver for the per-matrix smoke job. |
| `ea4-smoke-target.sh`      | Target-side install/load/serve/uninstall lifecycle. |
| `install-cpanel-on-guest.sh` | Operator script run **once** per VM to install cPanel + WHM and create the `01-cpanel-clean-el9` snapshot. |

## Scope

EL9 only. EL8 cPanel was dropped on 2026-05-21 — the upstream mod_pagespeed
binary is built once against Ubuntu's glibc (≥ 2.34) and will not load on
EL8's glibc 2.28. A companion `ea4-cpanel-elevate-test` job (EL8 → EL9
cpanel-elevate) used to live alongside this smoke rig; it was removed at
the same time because it had no path forward without an EL8 RPM.
Reintroducing EL8 requires a second `linux-release-build` cell with an EL8
base image — deferred.

## Operator workflow

1. **VM provisioning** (done — the internal planning notes Phase 0):
   - `CP-EL9-base` Hyper-V VM, AlmaLinux 9.7 from cloud image.
   - cloud-init seed sets hostname `cp-el9.lab.local` + 127.0.1.1
     loopback entry, key-only root SSH.
   - Baseline snapshot `00-clean-os` captured.

2. **cPanel install** (Phase 3, see install-cpanel-on-guest.sh):
   - Boot from `00-clean-os`, scp `install-cpanel-on-guest.sh`, run.
   - ~25–35 min. Auto-claims a 15-day trial license against
     the rig host's outbound IP.
   - Take Hyper-V snapshot `01-cpanel-clean-el9`.
   - Power off.

3. **Wire CI** (Phase 2 done):
   - `ea4-cpanel-smoke` runs on every release tag once the snapshot exists.
     `hv-check` step skips cleanly if the snapshot is missing.

## License model

- Trial: free, 15 days, auto-activates per IP. After expiry the cpanel
  service stops serving - the rig stops working.
- Solo: a paid per-IP license, tied to a fixed IP. The operator plan
  is to start with trial and switch to Solo before day 11 if the rig
  is healthy.

## Why ARP-by-MAC instead of `(Get-VM).NetworkAdapters.IPAddresses`

AlmaLinux cloud images do not install the `hypervkvpd` daemon by default,
so Hyper-V Integration Services cannot read the guest's IP via KVP. The
host's `Get-NetNeighbor -InterfaceAlias '*Default*'` table is populated
as soon as the guest sends a DHCP request, so we filter by the VM's MAC
address (`(Get-VMNetworkAdapter).MacAddress`) to discover the assigned
IP without depending on guest-side packages.

## License token path inside the guest

`pagespeed/kernel/license_v2/license_file.cc` derives the license file
location from `FileCachePath.parent_path() / "pagespeed.license"`. The
shipped `pagespeed.conf` (part of `ea-apache24-mod_pagespeed.rpm`) sets
`FileCachePath = /var/cache/mod_pagespeed`, so the license lands at
`/var/cache/pagespeed.license` with mode `0644` owned by `nobody:nobody`
(ea-apache24's default httpd user).
