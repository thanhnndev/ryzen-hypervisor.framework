# RyzenHV state on the GA403WM, 2026-11-16

The owner's machine is a GA403WM Hackintosh: Ryzen AI 9 HX 370 (family 0x1a model 0x24, 24 threads), macOS 26.7.2, OpenCore.
Branch: `research/ga403wm-zen5`. The owner judges this work by one thing: whether Docker (HyperKit, 24 vCPUs, 8 GB) runs smoothly
while they use the Mac.

## Live

- On the ESP, `EFI/OC/Kexts/RyzenHV.kext`: **0.7.22**, commit 319c2ff, UUID 56360452-841D-3EE0-A71B-DD91D5E8D22D, sha256 5c865167d6099032.
  Running since the boot of 2026-11-11 18:49, and again in the boot of 2026-11-16 18:39.
- Boot-args: `rhv_enable_svm=1 rhv_selftest=1 rhv_hv=1 rhv_allow_users=1 rhv_loglevel=4 -FIOff`. `-FIOff` is required: ForgedInvariant's
  per-core TSC writes caused the fq_codel panics.
- Backups in `EFI/OC/Kexts-backup/`:
  - `RyzenHV.kext.0.7.21-B37AAE98`: the first fallback.
  - `RyzenHV.kext.20261109-112219`: 0.7.20, verified.
  - `RyzenHV.kext.0718-broken`: kept as evidence of the mixed build.
  - Older 0.7.13, 0.7.16 and 0.7.17 copies.
- The ESP and config.plist are managed by the coordinator session (ga403wm-hackintosh `tools/occonfig.py`). Every install needs the
  owner's OK and is batched into a reboot.

## Proven on 0.7.22

The ASIDs and clean bits are described in `kext/Source/VmcbCache.hpp`.

- No panic since the broken 0.7.18 build (2026-11-09 10:16).
- The tsc check passes on 24 CPUs. vmFpuMismatches 0, vmInvalidExits 0.
- `hvf-smoke` passes. `hvf-exit-bench 2` without the shim:
  - one round trip costs 3.2 µs (cpuid 3.17, hlt 3.43, npt 3.18);
  - guest+switch per VMRUN is 0.41 µs, against 1.53 µs on 0.7.20;
  - the TLB is flushed on 0.4% of VMRUNs.
- Docker, 2026-11-16. Five compose containers ran: four were healthy, while minio (`object-storage`) restart-looped (its own config, not
  the VM). `docker run alpine` with curl got HTTP 200, and the guest's dmesg has no oops, segfault or stall.
  Over 20 s:
  - 317k exits/s, i.e. 13.2k per vCPU. 226k/s went to HyperKit, 161k/s of them nested page faults (xAPIC accesses); about 91k/s
    stayed in the kernel (host interrupts).
  - The guest TLB was flushed on 15% of VMRUNs, each one a new ASID after a vCPU moved to another core. Up to 0.7.21 it was 100%.
  - Per VMRUN: pre 0.78 µs, guest+switch 12.27 µs, post 0.36 µs. Front end 0.64 µs per trap.
  - hyperkit used 5.2 cores. About 3.9 of them were inside the guest, mostly the `pathcan-p0-pg` container at 217% CPU in the
    VM, i.e. guest work. The kernel's exit path took 0.5 cores and HyperKit's userland exit handling about 0.8.
  - A `sample` of hyperkit is dominated by `vlapic_write`: timer (`icrtmr`) and IPI (`icrlo`) writes, then `vmexit_inout`.

## Open

- **The main remaining cost is xAPIC emulation in HyperKit's userland**: about 160k nested page faults a second under load. The fixes
  are, in order of gain and risk:
  1. In-kernel handling of the APIC timer and ICR writes, or x2APIC MSR exits instead of MMIO, which saves the instruction decode.
  2. AVIC. It is supported here: features bit 13, 32768 ASIDs.

  Both are large and touch what HyperKit expects. Only worth it if Docker stops feeling smooth.
- Clean-bit hits are not counted; they show up only in the guest+switch time.
- HyperKit's raw user-client path (`sVmCreate`/`clientMemoryForType`) was wrong in the old mixed 0.7.17 build. It has been correct
  since 0.7.19, but it is unused by HyperKit and not exercised by any test.
- Not RyzenHV:
  - The minio restart loop.
  - An Android emulator `qemu-system-x86_64` at about 100% CPU on the host.
  - com.docker.backend at about 110%.
  - Shutdown stalls: an orphaned hyperkit after Docker quits.

## Rules learned the hard way

- Always `make kext-release`: a clean build, then `scripts/check-kext-symbols.sh` and `scripts/check-kext-abi.sh`. Never install an
  incremental build: stale objects with an old `RHVStats` layout panicked the Mac twice (0.7.18).
- A change to `RHVStats` bumps `RHV_STATS_SIZE`, keeps the old sizes accepted (V3/V2/V1), and updates `tests/host_test.c` and rhv-info.
