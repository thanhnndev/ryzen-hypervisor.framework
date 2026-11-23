# Where a Docker guest spends host CPU under RyzenHV, and what to change (2026-11-23)

Question (coordinator, 11-23): Docker is the only load that makes the GA403WM hot (62 W / 95 °C on 11-16 vs 17 W / 57 °C
today without it, `ga403wm-hackintosh` docs/POWER-UNDER-LOAD.md). How much of that is RyzenHV's exit path and how much
is the guest's own work? Which change saves the most watts?

Branch `research/exits-ranking` (worktree `~/develop/projects/rhv-wt-exits`), based on `research/guest-mem-lazy`
(0.7.23, installed). No kext code here; one measurement script, `tools/docker-exit-probe.sh` (no root).

## 1. What we know

### 1.1 Exit cost, today (`hvf-exit-bench 2`, 0.7.23, the desktop under light load)

| exit | rate, one vCPU | round trip | user | sys |
|---|---|---|---|---|
| CPUID | 208k/s | 4.80 µs | 1.08 | 3.72 |
| HLT | 274k/s | 3.65 µs | 0.81 | 2.82 |
| NPF (as an xAPIC access is) | 299k/s | 3.35 µs | 0.72 | 2.62 |

The kernel part from RyzenHV's own cycle counters (1.57 M exits, TSC 1996 MHz):

| step | per exit |
|---|---|
| pre (swap in) | 0.80 µs |
| guest + world switch | 0.49 µs |
| post (swap out) | 0.51 µs |
| front end in / out | 0.20 / 0.15 µs |
| whole trap, kernel side | 2.88 µs |

On 11-16 the bench gave 3.2 µs; today's 3.35-4.8 µs is with Edge video and Claude running.

These are the bench's minimum. A real HyperKit exit adds HyperKit's own handling on top: instruction fetch and decode for an
MMIO access, `vlapic_write`, the callout and the condition variable for HLT.

### 1.2 The Docker measurement of 11-16 (`docs/STATE.md`, 24 vCPUs, busy compose stack, EPP 0)

- **317k exits/s** (13.2k per vCPU), of which:
  - **226k/s went to HyperKit**: 161k/s nested page faults, which are xAPIC register accesses, and ~65k/s HLT, I/O,
    time slices and the rest;
  - **91k/s stayed in the kernel**: host interrupts.
- **hyperkit 5.2 cores**:
  - ~3.9 inside the guest. Mostly the `pathcan-p0-pg` container at 217 %: guest work, not overhead.
  - **0.5** in the kernel exit path.
  - **0.8** in HyperKit's userland exit handling. `sample`: `vlapic_write` (timer `icrtmr`, IPI `icrlo`), then
    `vmexit_inout`.
- Also on the host that boot: com.docker.backend ~1.1 cores, and kernel_task 108 %. The kernel_task load was
  memory-compression pressure: RyzenHV wired the whole 12.3 GB guest. **0.7.23 wires lazily** (guest ~3 GB instead of
  8 GB, `rhv-info hv reclaim 1` brings a live guest from 2.86 to 1.74 GB).

**So the exit path was ~1.3 cores of 5.2.** At EPP 0 that is about 5-6 W of the 62 W, and at today's EPP 191 about 3-4 W.
The other ~3.9 hyperkit cores were the guest doing work. Docker Desktop has run at **8 vCPUs / 8 GB** since 11-16, which
lowers the IPI and idle-timer exits (re-measure: section 4).

### 1.3 What HyperKit asks the hypervisor for (Docker Desktop 4.97, `nm -u com.docker.hyperkit`)

HyperKit calls only these:
- `hv_vm_create`, `hv_vm_map`, `hv_vm_protect`;
- `hv_vcpu_create`, `hv_vcpu_run`, `hv_vcpu_interrupt`;
- `hv_vcpu_read/write_register`, `hv_vmx_vcpu_read/write_vmcs`, `hv_vmx_read_capability`;
- `hv_vcpu_enable_native_msr`, `hv_vcpu_invalidate_tlb`.

It does **not** use `HV_VM_ACCEL_APIC` (macOS 12+, in-kernel LAPIC/IOAPIC/PIC/PIT), the `hv_vm_ioapic_*` and
`hv_vcpu_apic_*` calls, or `hv_vcpu_run_until`. Settings: `EngineTypeMac: hyperkit`, `UseVirtualizationFramework: false`,
`Cpus: 8`. HyperKit (bhyve's vmm) emulates the LAPIC, IOAPIC, PIC, PIT and HPET in its userland and handles HLT there:
`vm_handle_hlt` waits on a condition variable until its vlapic has an interrupt, and a timer callout or another vCPU
wakes it through `hv_vcpu_interrupt`. RyzenHV implements that as a kick IPI (`Vm.cpp` `kick()`, `mp_cpus_call`).

Consequence: **every APIC register access, every timer reprogram, every EOI and every IPI is a round trip to HyperKit.**
RyzenHV cannot handle any of them in the kernel while HyperKit owns the APIC state. That also rules out AVIC, whose backing
page *is* the APIC state.

### 1.4 This CPU (RyzenHV `rhv-info`, CPUID 8000_000A EDX = 0xfebfbdff)

| feature | bit | present |
|---|---|---|
| NPT | 0 | yes |
| NRIPS | 3 | yes |
| VMCB clean bits | 5 | yes |
| Flush by ASID | 6 | yes |
| DecodeAssists | 7 | yes |
| Pause filter + threshold | 10, 12 | yes |
| **AVIC** | 13 | yes |
| **x2AVIC** | 18 | yes |
| vNMI | 25 | yes |
| **Idle HLT intercept** | 30 | yes |

RyzenHV today:
- intercepts INTR, NMI, SMI, INIT, CPUID, INVD, HLT, IOIO and NPF, among others;
- has **no PAUSE intercept**;
- returns to userland at least every 10 ms (`kTimesliceNs`);
- keeps at most one kick IPI in flight per CPU.

## 2. Comparison

| | Linux KVM (SVM, this CPU) | Apple HVF on Intel Macs | RyzenHV + HyperKit today |
|---|---|---|---|
| LAPIC | in the kernel (`lapic.c`); xAPIC MMIO is an NPF that KVM decodes in the kernel (DecodeAssists) | in the client by default (HyperKit, QEMU); in the kernel with `HV_VM_ACCEL_APIC` (macOS 12+) | in HyperKit (no ACCEL_APIC client, and no ACCEL_APIC in RyzenHV) |
| Timer | hrtimer in the kernel; x2APIC TSC-deadline and ICR writes take the irq-off fast path (`handle_fastpath_set_msr_irqoff`): a few hundred ns, no return to user space | client callouts, or in the kernel with ACCEL_APIC | HyperKit callout; each `icrtmr` write is an NPF + a HyperKit decode |
| IPI | in-kernel ICR emulation and a kick; with **AVIC** (`kvm_amd.avic=1`; **x2AVIC** for x2APIC guests on Zen 4/5) an IPI to a running vCPU needs **no exit** at all (doorbell), and EOI/TPR are accelerated | client, or kernel with ACCEL_APIC | HyperKit `vlapic_write(icrlo)` + `hv_vcpu_interrupt` → kick IPI |
| HLT | the vCPU blocks in the kernel (`kvm_vcpu_halt`). Optional halt polling (`halt_poll_ns`, adaptive, default ≤ 200 µs) spends CPU to cut wake latency, so it is not a power feature (often set to 0 on laptops). Zen 5's idle-HLT intercept skips the exit when an interrupt is already pending (needs vINTR/AVIC) | client | exit to HyperKit, condition-variable block there (cheap per idle, one round trip per HLT) |
| Exits that reach user space | MMIO to real devices (virtio), I/O to emulated devices | client: APIC + devices; with ACCEL_APIC devices only | APIC + devices + HLT |

A Linux KVM guest on this CPU idles at near zero host CPU: timers fire in the kernel and HLT blocks there. A busy
guest's APIC traffic costs a few hundred ns per event instead of ~4-6 µs, or nothing with AVIC. That gap is RyzenHV's
headroom, but only for a VMM that lets the kernel own the APIC.

## 3. Changes, ranked by watts under the owner's Docker load (provisional)

The 11-16 overhead was ~1.3 cores (24 vCPUs, EPP 0) plus kernel_task compression ~1 core. The watt figures assume
~3 W per busy core at EPP 191 and are upper bounds until the 8-vCPU run (section 4) gives the real exit rates.

| # | Change | Saves (estimate) | Effort | Risk |
|---|---|---|---|---|
| 0 | **Container hygiene (owner side, no code).** On 11-16, `pathcan-p0-pg` ran at 217 % inside the guest (≈ 2.2 cores, ≈ 6-7 W) and minio restart-looped. If that is a stuck query or a loop and not real work, it is the largest single lever by far. `docker stats --no-stream` in the run below tells. | 0-7 W | none | none |
| 1 | **(d) reclaim on by default** (0.7.23 step 2). Lazy wiring already removed most of the 12.3 GB; reclaim trims an idle guest further (2.86 → 1.74 GB live). It pays only under memory pressure (Docker + emulator + browsers), where it avoids compression (kernel_task ~1 core on 11-16). | 0 W without pressure, ~3 W under it | tiny (default flip) | medium: kernel memory code; needs a soak (Docker + build + `hv reclaim` stats) before default |
| 2 | **x2APIC for the HyperKit guest** (in-memory HyperKit patch like the existing ones: report CPUID x2APIC and start vCPUs with `X2APIC_ENABLED`; bhyve's vlapic has an x2APIC MSR path). Then APIC accesses become MSR exits: HyperKit no longer fetches and decodes the instruction (a guest page walk per access), and an IPI is one ICR write instead of ICR_HI + ICR_LO. | ~30-40 % of HyperKit's 0.8-core userland share ≈ 1 W busy, less idle | small-medium (pattern patch + test) | medium: guest APIC mode change. Without interrupt remapping, Linux enables x2APIC only if the hypervisor says it may (`x86_init.hyper.x2apic_available()`; Linux has a bhyve guest driver that reads a bhyve CPUID feature leaf). Check the guest kernel and `dmesg` first: if it prints "IRQ remapping doesn't support X2APIC mode", this item is void |
| 3 | **Cheaper exit in RyzenHV**: pre + post is 1.3 µs of the 3.35 µs round trip. Keep host MSR state and the guest FPU loaded across consecutive exits of the same vCPU thread, and lazy-restore on return to user space. At ~300k exits/s that is up to 0.2 core. | ≈ 0.5 W | medium | low-medium (FPU/MSR state bugs are the panic class of 0.7.18) |
| 4 | **(e) vCPU count advice: 8 → 4-6** for daily use. The idle cost per vCPU is its timer ticks + HLT round trips. IPI fan-out falls with fewer vCPUs. | idle overhead −25-50 % (re-measure) | none | build throughput |
| 5 | **(b) in-kernel LAPIC/IOAPIC/PIT = `HV_VM_ACCEL_APIC` in RyzenHV**, and a VMM that uses it. HyperKit does not, so this means Docker on Virtualization.framework (`UseVirtualizationFramework: true`). Virtualization.framework on Intel most likely runs on Hypervisor.framework with `HV_VM_ACCEL_APIC`, since the API arrived with it in macOS 12; not verified here, which must then work through our front end. APIC, timer and HLT then stay in the kernel: ~70-80 % of today's user-space traps disappear. | ≈ 0.6-1.0 core busy ≈ 2-3 W, and most of the idle cost | large (weeks): APIC + IOAPIC + PIC + PIT + `run_until` + VZ bring-up | high: new interrupt and timer code in the kernel; VZ on AMD is untested |
| 6 | **(a) HLT blocking in the kernel**: only possible with 5 (the kernel must know when the APIC has an interrupt). Alone, under HyperKit, it saves one round trip per idle period; HyperKit already blocks, it does not spin. | < 0.3 W alone | small with 5, impossible without | low with 5 |
| 7 | **(c) AVIC / x2AVIC**: on top of 5, IPIs to running vCPUs and EOIs without exits. Idle HLT intercept on top of that. | +0.5-1 W over 5 under busy multi-vCPU load | large | high (AVIC state, IOMMU-less doorbells, wakeups of blocked vCPUs) |
| - | Halt polling | negative for power (trades CPU for latency) | | not proposed |
| - | Timer coalescing / lower QoS for hyperkit | small and risky for guest timekeeping | | not proposed |

**Provisional reading.** RyzenHV's exit path is a few watts, not the 45 W step. The step is mostly the guest's and the
host's own work, plus compression that 0.7.23 largely removed. The cheap and safe order:
1. find out whether the 217 % container is real work (0);
2. soak and then default reclaim (1);
3. x2APIC for HyperKit's guest (2).

The in-kernel APIC (5, 6, 7) is the Linux-class fix, but it only helps through a VMM that uses it, and it costs weeks
for ~2-4 W.

## 4. The one Docker run needed (coordinator starts Docker; a few minutes; no root except the optional sample)

The owner's normal Docker state (compose stack up, as during work), then hands off Docker for ~2 minutes:
```
open -a Docker                                   # if not running; wait until `docker info` answers and the stack is up, ~2 min
~/develop/projects/rhv-wt-exits/tools/docker-exit-probe.sh 60 docker-idle > /tmp/rhv-docker-idle.txt
docker stats --no-stream > /tmp/rhv-docker-stats.txt
```
Optional, one admin dialog, 10 s: where HyperKit's userland time goes on the 8-vCPU guest:
```
sudo sample "$(pgrep -f 'com.docker.hyperkit' | head -1)" 10 -file /var/tmp/hyperkit.sample.txt
```
The probe prints:
- exits/s, split into to-user, NPF-to-user (xAPIC) and in-kernel;
- per-VMRUN and per-trap µs;
- TLB flushes, new ASIDs, lazy faults, reclaim;
- package W and Tctl;
- top of the VM processes over the same window.

Leave Docker as the owner wants it afterwards. With those numbers the watt column of section 3 becomes measured, and
item 0 is answered.

## 5. Measured: the owner's stack idle on 8 vCPUs (2026-11-23 15:09, 60 s, after 2 min settle)

Stack: postgres, valkey, clamav, and object-storage (minio), which is still restart-looping. The pathcan backend and the
emulator were not running. Raw output: `docs/measurements/2026-11-23-docker-*.txt`.

| | value |
|---|---|
| exits/s | **7,248**: 6,955 to HyperKit (5,052 xAPIC NPF, 1,903 HLT/IO/other), 293 in the kernel |
| hyperkit host CPU | **17.5 % of one core** (11-09, 24 vCPUs, idle containers: 73-124 %) |
| package | 15.3 W (light desktop load alongside; 17.0 W at 14:54 without Docker, so within the noise) |
| per VMRUN | pre 1.13 µs, guest+switch 14.0 µs, post 0.52 µs; front end 0.40 / 0.45 µs per trap |
| TLB flushes | 445/s, each a new ASID after a vCPU moved to another core |
| containers (`docker stats`) | postgres 8.1 % (health checks), valkey 0.3 %, clamav 0.0 %, object-storage restarting |

Guest kernel command line (from the hyperkit arguments): `noapic tsc=reliable ...`. So the guest uses the legacy PIC
instead of an IOAPIC. The per-vCPU LAPIC is still emulated by HyperKit: those are the 5k NPF/s.

**Reading.**
- An idle Docker stack now costs **about 0.2 core ≈ 0.5-1 W**. The 8-vCPU setting removed most of the 11-09 idle cost.
  The exit work in it: 6,955 traps × ~4-5 µs ≈ 3 % of a core in round trips, plus HyperKit's own handling.
- Idle Docker is **not a heat source any more**. The 62 W / 95 °C of 11-16 was the stack doing work:
  - `pathcan-p0-pg` at 217 % inside the guest;
  - the native Python backend at 3.1 cores;
  - the emulator at 1 core;
  - at EPP 0, with compression from the fully wired 12.3 GB guest.
- Under that kind of load, RyzenHV's share is the ~1.3 cores measured on 11-16 with 24 vCPUs, less at 8.

## 6. Ranking after the measurement

| # | Change | Watts at the owner's loads | Effort / risk |
|---|---|---|---|
| 1 | **Measure a busy run before any kernel work**: the same probe while the owner's backend and tests run. Busy is the only state where the exit path is worth more than ~1 W. | n/a | none |
| 2 | **Reclaim on by default** (0.7.23): keeps the guest's wired memory small when Docker, the emulator and browsers run together. That pressure produced the 11-16 compression (≈ 1 core). | 0 W idle, ~3 W under pressure | tiny / medium (soak first) |
| 3 | **x2APIC for HyperKit's guest** (only if the LinuxKit guest accepts x2APIC without interrupt remapping). | idle < 0.2 W; busy ≈ 1 W | small-medium / medium |
| 4 | **Cheaper RyzenHV exit** (pre is 1.13 µs here) | busy ≈ 0.5 W | medium / low-medium |
| 5 | **In-kernel APIC + VZ, then in-kernel HLT handling, then AVIC** | idle ≈ 0.3-0.5 W; busy 2-4 W | weeks / high |
| owner | object-storage (minio) restart loop, unchanged since 11-16 | small CPU, but a container start every few seconds | owner's compose config |

**Bottom line:** nothing in RyzenHV is worth a kernel change for heat at idle. The next useful step is the busy-run
measurement (1) with the owner's real work, then 2.
