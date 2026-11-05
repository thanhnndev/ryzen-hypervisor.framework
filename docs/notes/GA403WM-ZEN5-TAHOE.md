# RyzenHV trên ASUS GA403WM (Ryzen AI 9 HX 370, Zen 5) với macOS 26.7.1

Ghi chép nghiên cứu, ngày 2026-11-04, ở commit `4e28aba` (0.7.8). Chưa nạp kext, chưa sửa EFI hay NVRAM, chưa chạy sudo.
Mọi số liệu về CPU dưới đây đo bằng lệnh `cpuid`/`xgetbv` từ userland, kernel đọc bằng `nm`/`otool` trên file
`/System/Library/Kernels/kernel` (không chạm vào kernel đang chạy).

## 1. Kiến trúc

```
 ứng dụng (QEMU, HyperKit, Parallels, VirtualBox)
     │  API Hypervisor.framework (kiểu VMX: VMCS field, VMX exit reason)
     ▼
 front end ── A. native (rhv_hv=1): kext đăng ký với XNU qua hv_set_traps / hv_set_callbacks,
          │      framework thật của Apple gọi vào kext            kext/Source/HvFrontend.cpp
          └─ B. shim: dylib chèn bằng DYLD_INSERT_LIBRARIES, thay hàm hv_* bằng dyld interpose,
                 nói chuyện với kext qua IOUserClient            shim/hv_shim.c, shim/rhv_core.c, shim/rhv_iokit.c
     ▼
 lõi (kext)
   VMCS giả trên VMCB             VmcsState.hpp, VmcsFields.def, VmxMap.hpp
   VM / vCPU, vòng VMRUN          Vm.cpp, SvmRun.S
   bộ nhớ: hv_vm_map → NPT        Npt.hpp, PhysMem.cpp
   bật SVM từng core              SVM.cpp
```

### 1.1 Bật SVM (SVM.cpp)
- `probe()` chạy `mp_rendezvous_no_intrs` trên mọi core: CPUID `0x80000001 ECX[2]` (SVM), `0x8000000A` (revision,
  số ASID, feature EDX), đọc `VM_CR` (`0xC0010114`, bit 4 SVMDIS, bit 3 LOCK) và `EFER` (APM Vol.2 §15.4, §15.30.1).
- `enableAll()`: mỗi core một trang host save area (4 KiB, liên tục), rồi `EFER.SVME=1` (bit 12) và ghi `VM_HSAVE_PA`
  (`0xC0010117`, APM §15.30.4) bằng read-modify-write.
- Trên macOS 26 các core có thể chưa online khi kext start, nên có `rescan()` (0.7.4) và `ensureThisCpu()`: trước mỗi
  `VMRUN`, với ngắt đã tắt, kext đọc lại EFER/VM_HSAVE_PA của core hiện tại và sửa nếu sai (`CLGI` khi SVME=0 là #UD,
  tức là panic).
- Kext chỉ đụng tới 4 MSR: `EFER`, `VM_CR`, `VM_HSAVE_PA`, `TSC_AUX` (TSC_AUX chỉ khi CPUID báo có RDTSCP). Cả 4 đều
  là MSR kiến trúc, có trên Zen 5.

### 1.2 VMRUN (SvmRun.S, Vm.cpp ~860–1060)
`CLGI → VMSAVE host → VMLOAD guest → nạp GPR → STI → VMRUN → CLI → lưu GPR → VMSAVE guest → VMLOAD host → STGI`.
Ngoài phần cứng tự lo, kext tự đổi: x87/SSE/AVX + XCR0 (`XSAVE/XSETBV/XRSTOR`), DR0–DR3/DR6/DR7 (chế độ VMX),
`TSC_AUX`, CR2 của host. Mỗi lần chạy có timeslice khoảng 10 ms để thread của client vẫn bị ngắt được.

### 1.3 Dịch VMX → SVM
- Client đọc/ghi field VMCS theo mã số; `VmcsState.hpp` ánh xạ từng field sang offset VMCB (`VMCB.hpp` có
  `static_assert` cho từng offset, theo APM Appendix B) hoặc giữ trong một "bóng" VMCS.
- Exit SVM được chuyển thành VMX exit reason + qualification (`VmxMap.hpp`): ví dụ NPF `0x400` → EPT violation 48,
  với bit "linear address valid" lấy từ `EXITINFO1` bit 32/33; `0x8D` XSETBV → 55.
- Các bit hợp lệ trên VMX nhưng sai trên AMD được giữ trong field và loại khỏi VMCB: `CR4.VMXE`, `CR0.NW` khi không có
  `CD` (0.7.8).
- `EFER.SVME` của guest luôn bật trong VMCB và bị ẩn khi client đọc (VmcsState.hpp:167, 211, 303); VMRUN đòi bit này
  (APM §15.5.1).
- Capability VMX (`hv_vmx_read_capability`) được tổng hợp, kể cả TPR shadow (bit 21 của primary controls) từ 0.7.3.

### 1.4 NPT (Npt.hpp)
Bảng 4 cấp, chỉ trang 4 KiB, GPA tới 2^48. Entry là PTE long mode bình thường (APM §15.25); US phải bật ở mọi cấp vì
lượt duyệt nested luôn tính là truy cập user; quyền thật nằm ở leaf (P/RW/NX). Trang của process được ghim bằng
`IOMemoryDescriptor::prepare`. Mọi VM dùng ASID 1 (Vm.cpp:665) và mỗi VMRUN đều `TLB_CONTROL = 1` (xoá toàn bộ TLB,
Vm.cpp:902), không dùng clean bits.

### 1.5 Hai front end
- **Shim (B):** bọc toàn bộ API, cache thanh ghi/field giữa các exit, một syscall mỗi exit (ABI 8, `VmRunEx`). Cần
  AMFI nới lỏng vì phải chèn dylib vào app.
- **Native (A):** XNU có sẵn giao diện cho "hv support kext" (`hv_set_traps`, `hv_set_callbacks`,
  `hv_set_task_target`...). Kext đăng ký vào đó thay cho AppleHV, nên Hypervisor.framework thật gọi trap vào kext.
  Thêm vào đó: ghi `kern.hv_support` bằng địa chỉ giải mã từ mã của `hv_get_support()` (mẫu byte
  `55 48 89 e5 8b 05`), vá 2 nhánh trong bộ nhớ của HyperKit (`HvPatch.cpp`), chỉnh CPUID cho HyperKit và Parallels.

## 2. Độ tương thích với máy này

### 2.1 CPU (đo thực tế)

| mục | giá trị | ý nghĩa với RyzenHV |
|---|---|---|
| CPUID 1 EAX | `0x00B20F40` → family `0x1A`, model `0x24` | kext không kiểm tra family, không chặn |
| SVM (`8000_0001 ECX[2]`) | 1 | có |
| SVM revision / ASID | 1 / 32768 | như Zen 3 |
| `8000_000A EDX` | `0xFEBFBDFF`: NP, NRIPS, VmcbClean, FlushByAsid, DecodeAssists, PauseFilter(+Threshold), AVIC, x2AVIC, VGIF, VNMI, VMSAVEvirt, SpecCtrl, … | đủ thứ kext cần (NP, NRIPS); AVIC/x2AVIC/VNMI có nhưng kext chưa dùng |
| SEV/SNP | SME=1, SEV=0 | không liên quan |
| địa chỉ vật lý/ảo | 48/48 bit, không LA57 | khớp NPT 4 cấp, `kGpaLimit = 2^48` |
| AVX-512 | có (`leaf7 EBX[16]`), `hw.optional.avx512f = 1` | **xem 2.3** |
| `CPUID.0xD.0` | XCR0 hỗ trợ `0x2E7`, kích thước tối đa 2440 byte | nhỏ hơn `RHV_FPU_AREA_MAX = 3072` và trang 4096 của kext |
| luồng | 24 logical / 12 core (Zen 5 + Zen 5c) | dưới `RHV_MAX_CPUS = 64`; kext đòi mọi core usable, Zen 5c cùng ISA nên lẽ ra đạt, cần kiểm bằng `rhv-info` |

Các bit `VM_CR.SVMDIS`/`LOCK` chỉ đọc được trong kernel: biết được sau khi nạp kext (`rhv-info` dòng `BIOS disable`).

### 2.2 macOS 26.7.1 (25G241, xnu-12377.161.15.700.19)
- `hv_get_support`, `hv_set_traps`, `hv_release_traps`, `hv_set_callbacks`, `hv_set_{task,thread}_target`,
  `hv_get_{task,thread}_target`, `hv_support_available`, `mp_rendezvous_no_intrs`, `cpu_number` đều có trong kernel.
- `hv_get_support` disassemble ra `push rbp; mov rbp,rsp; mov eax,[rip+hv_support_available]; pop rbp; ret`, khớp mẫu
  byte trong HvFrontend.cpp:1462, nên `rhv-info hv sysctl 1` có lẽ dùng được.
- `make kext`: `check-kext-symbols: all 401 imported symbols resolve against this kernel`.
- Issue 1 (macOS 26.7.1, 5900X): core lên muộn. Đã có `rescan()` từ 0.7.4 và người báo lỗi xác nhận chạy được.
- Issue 13 (macOS 26.7.1): HyperKit bị giết vì `Code Signature Invalid` sau khi bị vá, máy đó còn bật SIP. Máy này có
  `amfi_get_out_of_my_way=0x1`, `amfi=0x80`, AMFIPass.kext và `csr-active-config = 0x0A43`. Phải xem dòng
  `signing flags of com.docker.hyperkit … enforcement off` trong `rhv-info log` mới biết chắc.

### 2.3 Rủi ro riêng của Zen 4/5: XCR0 thay đổi theo từng thread
Đo trên máy này: một process mới có XCR0 = `0x7`. Sau **một** lệnh AVX-512 (`vpxord zmm`), XCR0 của thread đó lên
`0xE7`. XNU bật AVX-512 trễ, riêng cho từng thread, qua trap #UD (`fpUDflt` có trong kernel 25.6). Zen 3 (5800H của
tác giả) không có AVX-512 nên chưa bao giờ gặp trường hợp này.

Code liên quan:
- `Vm.cpp:607–613`: kích thước vùng FPU (`fFpuSize`) lấy từ `CPUID.0xD.0 EBX` **của thread tạo VM** (832 nếu XCR0=7,
  2440 nếu đã lên AVX-512).
- `Vm.cpp:964–971`: nếu XCR0 của guest có bit mà thread đang chạy vCPU chưa bật, `VmRun` trả `kIOReturnUnsupported`
  ("guest XCR0 … needs state this thread has not enabled") và vCPU dừng.
- `Vm.cpp:1227`: `set_fpstate` đòi `size == fFpuSize`.

QEMU (`target/i386/hvf/x86_cpuid.c`, bản master: `hvf_get_supported_cpuid`, `cache_host_xcr0`) cho phép các bit
AVX-512 ở leaf 7, nhưng leaf 0xD lại lấy từ `xgetbv` của chính process QEMU. Nếu lúc đó XCR0 là 7 thì Linux guest không
bật AVX-512, an toàn. Nếu thread của QEMU đã lên AVX-512 trước đó, guest sẽ `XSETBV 0xE7`, trong khi thread vCPU có thể
vẫn ở 7 → VM dừng. HyperKit và Parallels chưa được kiểm. **Đây là giả thuyết từ code, chưa chứng minh trên phần cứng**:
cách kiểm ở bước 5 của kế hoạch.

### 2.4 Bản vá AMD của OpenCore
Cấu hình đang dùng có bộ AMD Vanilla (cores per package, `cpuid_set_generic_info`, bỏ qua kiểm tra GenuineIntel,
`CPUFAMILY_INTEL_PENRYN`, `mtrr_update_action` sửa PAT, bỏ panic non-monotonic time). Không có `Emulate → Cpuid1Data`,
nên lệnh `cpuid` (của kext và của userland) thấy đúng AMD thật. Kext đọc CPUID trực tiếp, không qua `machdep.cpu`, nên
các bản vá đó không ảnh hưởng tới phần dò SVM. Bản vá PAT ảnh hưởng PAT của host; leaf NPT để PAT index 0 (WB). Chưa
thấy chỗ nào xung đột.

`NVRAM → Delete` có `boot-args` và `WriteFlash = true`: boot-args của USB thử nghiệm sẽ được EFI chính ghi đè ở lần boot
sau, nên không cần dọn NVRAM bằng tay.

### 2.5 tests/ ở userland
`make test` chạy được hết trên máy này (host_test, vmcb_layout, npt_test, vmx_map_test với 166 field so với SDK,
vmcs_state_test, fpu_state_test, msr_map_test, patch_test, shim_core_test: tất cả OK). Hai hạn chế:
- `fpu_state_test` chỉ chạy với XCR0 = `0x7` / 832 byte của thread test, nên không kiểm được trường hợp ở 2.3.
- `patch_test` bỏ qua phần chạy trên file thật của Parallels/HyperKit vì chưa cài.
`hvf-smoke`, `rhv-guest-test`, `rhv-map-test`, `rhv-vmx-test`, `rhv-long-test` cần kext đã nạp.

## 3. Kế hoạch thử an toàn

Các bước có (**hỏi trước**) đụng tới kernel, EFI hoặc cần sudo, chỉ làm khi bạn đồng ý.

0. **Chuẩn bị (đã làm):** `make test`, `make kext tools shim` → `build/RyzenHV.kext`, `build/rhv-info`,
   `build/hvf-smoke`, `build/libryzenhv.dylib`.
1. **USB EFI riêng (hỏi trước):** USB GPT + FAT32, chép `ga403wm-hackintosh/efi/EFI` sang. Chỉ sửa
   `config.plist` của USB:
   - `Kernel → Add`: thêm RyzenHV.kext **sau cùng** (Arch Any, ExecutablePath `Contents/MacOS/RyzenHV`, PlistPath
     `Contents/Info.plist`).
   - `Misc → Debug → Target = 67` để OpenCore ghi log lên USB (bắt lỗi `Prelinked injection … Invalid Parameter`).
   - boot-args của lần đầu: giữ nguyên + `-v` (đã có `keepsyms=1 debug=0x100`). **Chưa** thêm `rhv_enable_svm`.
   Không dùng `./rhv kext` vì nó tự mount EFI bằng sudo và tự sửa config; sửa tay USB dễ kiểm soát hơn.
2. **BIOS (bạn làm):** bật SVM Mode (trên máy ASUS thường ở Advanced → CPU Configuration). Boot từ EFI chính một lần
   để chắc rằng việc bật SVM không làm hỏng gì.
3. **Boot USB, chỉ probe:** `./build/rhv-info` phải ra family 0x1a model 0x24, SVM supported, BIOS disable NO, NPT [x],
   usable = 24. `./build/rhv-info log`. Nếu boot lỗi: dùng `rhv_stage=0..5 rhv_slow=3` để chia đôi (docs/DEBUGGING.md),
   hoặc `-rhv_off`, hoặc boot EFI chính.
4. **Bật SVM:** thêm `rhv_enable_svm=1 rhv_selftest=1`. Log phải có `SVM enabled on 24 CPUs`, `selftest: 24 of 24
   CPUs ok` và dòng `rescan:`; (**hỏi trước**) `sudo ./build/rhv-info selftest`. Dùng máy bình thường khoảng 1 giờ.
   Không cho máy ngủ trong cả đợt thử (sleep/wake chưa được kiểm, và máy này dùng S0ix).
5. **Native + QEMU:** thêm `rhv_hv=1 rhv_allow_users=1`. `./build/rhv-info hv` phải báo `front end: registered`.
   (**hỏi trước**) `./build/rhv-info hv sysctl 1` (ghi biến kernel) → `sysctl kern.hv_support` = 1.
   `./build/hvf-smoke`. Sau đó dùng QEMU 11.1.2 của MacPorts (cùng bản tác giả đã thử; đã có entitlement
   `com.apple.security.hypervisor`) để boot Alpine netboot `vmlinuz-virt` + `initramfs-virt`:
   `qemu-system-x86_64 -accel hvf -cpu host -m 1024 -smp 2 -nographic -kernel … -initrd … -append console=ttyS0`.
   Kiểm tra AVX-512 (2.3): trong guest chạy `grep -c avx512f /proc/cpuinfo` và `dmesg | grep -i xsave`; trên host chạy
   `rhv-info log | grep "needs state"`. Nếu VM dừng ở đây thì thử lại với `-cpu host,-avx512f` để xác nhận nguyên nhân.
6. **`rhv doctor`:** script này chỉ chạy đúng trong archive release (cần `VERSION`, `bin/rhv-info`, `qemu/`). Tải
   archive 0.7.8 cùng commit (ABI giống nhau) và chạy `./rhv doctor` từ đó. Kext vẫn là bản tự build. Dòng cảnh báo
   "macOS 26: tested on 15.8.1 only" là bình thường.
7. **Docker, làm lần lượt:**
   a. `./rhv docker up` (từ archive): VM Alpine trong QEMU + dockerd, không vá process nào. Ít rủi ro nhất.
   b. Colima/Lima với `--vm-type qemu` (MacPorts có port), cũng đi qua QEMU hvf.
   c. Docker Desktop 4.97.0 bản Intel (HyperKit, kext vá bộ nhớ process): trước đó xem `rhv-info log | grep "client
      fix-ups"` phải báo `enforcement off`. Xem thêm ROADMAP mục 13 và 19.
8. **Parallels (tuỳ chọn, để sau cùng):** luôn tắt 3D vì đang dùng driver NVIDIA Metal bên thứ ba (README).

Trước khi khởi động lại máy, tắt hết VM. Chỉ dùng một chương trình VM tại một thời điểm.

## 4. Lỗi và điểm có thể cải thiện (để đóng góp)

| # | loại | chỗ | nội dung |
|---|---|---|---|
| 1 | bug khả nghi (Zen 4/5) | Vm.cpp:607–613, 964–971, 1227 | XCR0 theo từng thread (2.3). Hướng sửa cần bàn: chặn AVX-512 khỏi XCR0 mà client được báo (`fXcr0Supported` &= XCR0 của host khi tạo VM), hoặc ép thread vCPU lên AVX-512 trước khi chạy (shim: chạy một lệnh AVX-512 ở userland; native: cần một cơ chế trong kernel), và bỏ yêu cầu `size == fFpuSize` cố định theo thread tạo VM. Trước hết cần một test trên phần cứng. |
| 2 | test | tests/fpu_state_test.cpp | Thêm trường hợp XCR0 có AVX-512 (`0xE7`, 2440 byte); test có thể tự lên AVX-512 bằng một lệnh `vpxord zmm` khi `hw.optional.avx512f=1`. |
| 3 | hiệu năng | Vm.cpp:665, 902–903 | Một ASID cho mọi VM và xoá toàn bộ TLB mỗi lần VMRUN. CPU này báo có FlushByAsid (`TLB_CONTROL = 3`, APM §15.16) và VmcbClean (§15.15): ASID riêng cho từng VM/vCPU + clean bits sẽ giảm chi phí mỗi exit. |
| 4 | an ninh | cả kext | Không có xử lý `SPEC_CTRL`/IBPB giữa guest và host (grep không thấy MSR 0x48/0x49). Zen 5 có V_SPEC_CTRL (`8000_000A EDX[20]`) và AutoIBRS (`8000_0021 EAX[8]`). |
| 5 | tương thích | HvFrontend.cpp:937–942 | CPUID của HyperKit chỉ được dọn ở `0x80000001` và `0x80000008`. Zen 5 báo leaf mở rộng tới `0x80000028` (AutoIBRS, topology `0x80000026`…), chưa được thử với Linux guest trong HyperKit. |
| 6 | công cụ | release/package.sh:22–28 | Không đóng gói được nếu máy không cài Parallels (bước copy `libPrlXmlModel` là bắt buộc). Nên bỏ qua phần Parallels khi không có, hoặc thêm chế độ "dev bundle" để `rhv doctor` chạy được từ cây source. |
| 7 | công cụ | release/rhv:64–65 | `doctor` cảnh báo mọi macOS khác 15.x. Nên ghi rõ "26.x: báo cáo từ người dùng, issue 1". |
| 8 | tài liệu | docs/RYZENTOSH.md:72 | Chỉ nêu "family 0x19 model 0x50". Thêm bảng ví dụ: Zen 2 0x17, Zen 3 0x19/0x50, Zen 5 Strix Point 0x1A/0x24. |
| 9 | tài liệu | docs/ROADMAP.md (Phase 0) | Ghi lại kết quả build trên macOS 26.7.1 / SDK 26.5: 401 symbol resolve, `hv_get_support` khớp mẫu byte. |
| 10 | build | shim/hv_shim.c:552, 595; kext `-Wsign-conversion` | Cảnh báo nhỏ: `madt_dump`, `spin_dump` không dùng; vài chỗ đổi dấu ngầm. |
| 11 | tính năng | — | Zen 5 có AVIC/x2AVIC/VNMI; README nêu là chưa có in-kernel APIC. Đây là việc lớn, chỉ ghi nhận. |
