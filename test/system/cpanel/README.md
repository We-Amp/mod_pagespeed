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
                │   scp RPM/sh ─────────┘                        │
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

EL9 + **EL8** (2026-06-08). EL8 cPanel was dropped on 2026-05-21
(the dev-image Apache `.so` is built against Ubuntu glibc ≥ 2.34 and would not
load on EL8's glibc 2.28) and **revived** once `apache-el8-build` could rebuild
`//:libmod_pagespeed.so` against EL8 glibc 2.28 (almalinux:8 + gcc-toolset-13,
static libstdc++, floor-gated ≤ 2.28). The `ea4-cpanel-smoke` matrix is now
`[el9, el8]`; the el8 cell runs against `CP-EL8-base` / `01-cpanel-clean-el8`
(both still present on the rig host — only the matrix cell had been removed).
`ci-ea4-smoke.ps1` accepts `-Os el8`.

**el8 is a REQUIRED peer of el9** (owner decision 2026-06-08), not an optional
add-on: the release refuses to publish a partial (el9-only) EA4 set and the
prod-promote refuses an `ea4/el9` tree without a matching `ea4/el8` tree —
required-peer enforcement, so a half-published set can never reach customers.

**CloudLinux caveat:** the el8 smoke validates stock-EA4 Apache on AlmaLinux 8.
CloudLinux 8's `cl-ea4`-patched Apache + `mod_lsapi` + PHP Selector is a
**tracked follow-up** (needs a CloudLinux license/VM) — Gate-2 (AlmaLinux 8)
gates the el8 prod-promote; Gate-3 (real CloudLinux) is validated separately
and does NOT block the el8 prod-promote.

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

## cPanel license (guest VM dependency)

This is cPanel's own product license for the guest — nothing the module
needs (mod_pagespeed carries no license apparatus). It matters only because
an unlicensed cPanel stops serving, which takes the rig down with it:

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
