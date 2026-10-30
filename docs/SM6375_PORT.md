# SM6375 port plan — OnePlus Nord CE 3 Lite 5G

Goal: run the Bergamota iOS 18.7.8 userland on a Snapdragon 695 5G (SM6375)
device, for fun. This is a **new board bring-up**, not a recompile. The plan
below maps every step onto the existing `develop` tree so the work is concrete.

Last updated: 2026-10-30.

---

## 1. Honest scope

The Bergamota architecture is the right one for this SoC, and SM6375 is in
several ways a *friendlier* target than the Poco F1 (SDM845) that `develop`
currently targets:

- **Cortex-A78 is ARMv8.2-A: no pointer authentication.** Renegade (the
  arm64e → arm64 PAC-strip + root builder) works unchanged. This is the single
  biggest risk of the whole project and it is already solved.
- **Adreno 619 is a6xx**, so the Chlorophyll-on-Turnip (Mesa Turnip) GPU work
  ports with less friction than it did to the A630.

But this is still a fresh silicon bring-up. The SDM845 port is ~900 commits /
~2 years to SpringBoard on one phone. A realistic target for SM6375 is
**milestone-by-milestone** (serial console first, then ramdisk, then
SpringBoard), not "a working phone". After 2 years the Poco F1 still has no
SEP/Face ID, camera, audio, and only recently gained Wi-Fi + GPU.

Treat the following as the definition of success for this project:

| Stage | Success criterion |
|---|---|
| M0 | Stock boot image + live device tree captured from the Nord |
| M1 | Enxerto boots on the Nord and shows its console on the panel |
| M2 | Heartwood (xnu) boots to a serial/framebuffer console |
| M3 | iOS root builds with Renegade (unchanged) and mounts as root |
| M4 | SpringBoard reaches the home screen (CPU-rendered) |
| M5 | Adreno 619 GPU / Turnip (stretch) |

---

## 2. Device facts

### Captured live (M0 done, 2026-10-30)

Pulled from the device (LineageOS 24 "larry", Android 17, userdebug, `adb root`):

| Item | Value |
|---|---|
| SoC | SM6375, codename **`blair`**; `qcom,msm-id` = `0x1fb` (507), `qcom,board-id` = `0xb` (11) |
| Model | "Qualcomm Technologies, Inc. Blair QRD NOPMI" (LineageOS uses the QRD DT) |
| CPU | 8 cores, `qcom,kryo`, PSCI, release-addr `0x50000000`. **6 silver** `cpu@0`–`cpu@500` (capacity 0x400) + **2 gold** `cpu@600`–`cpu@700` (capacity 0x79a) |
| GIC | GICv3 at **`0xf200000`** (GICD, 64 KiB), GICR **`0xf240000`** (1 MiB), redistributor stride `0x20000` |
| Generic timer | 19.2 MHz; `arm,armv8-timer`; vtimer = PPI 3 → **INTID 19** (same as sweet/beryllium) |
| UFS | `ufshc@4804000` → **`0x04804000`** (size `0x3000`); UFS ICE at `0x4808000` |
| USB | `ssusb@4e00000` → **`0x04e00000`** (DWC3) |
| Framebuffer | `splash_region@0x85200000` → **`0x85200000`** (12 MiB, "cont_splash_region") |
| IMEM | `qcom,msm-imem@c125000` → **`0xc125000`**; `restart_reason@65c` → **`0xc12565c`** |
| PSHOLD | `restart@440b000` (compatible `qcom,pshold`) → **`0x440b000`** (size 4) |
| PMIC | **PM6125** + **PMK8350** on SPMI (`qcom,spmi@1c40000`) |
| Touch | **Novatek NT36672C** (`novatek,nf_nt36672c`) on `spi@4a80000` |
| Panel | **NT36672C Tianma FHD**, video mode (`qcom,mdss_dsi_oplus22821_nt36672c_tianma_fhd_video`), 1080×2400 |
| Display engine | `qcom,mdss_mdp@5e00000`, compatible **`qcom,sde-kms`** (the same mainline SDE subsystem SDM845 uses) |
| WLAN/BT | **WCN3990** (`slim@a5c0000` / `wcn3990`) — same chip as beryllium |
| RAM (8 GB unit) | `0x80000000`+`0x3bb00000`, `0xc0000000`+`0xc0000000`, `0x180000000`+`0x100000000` |
| Boot image | header **v3**, virtual A/B (active slot `_b`), `dtbo_idx=2`, verified boot `orange` (unlocked) |
| DTB source | merged live tree `/sys/firmware/fdt` (688156 bytes), dumped to `dtb/sm6375/live.dump.txt` |

Fixed carve-outs in the low bank run `0x80000000` (hyp) through ~`0x9d600000`
(cdsp@0x9b800000 + 0x1e00000), so the kernel must live above that.

### Key corrections vs. the first draft of this plan

The live DT made the port **easier** than first estimated in three places:

1. **Display is `qcom,sde-kms`, not a foreign DPU.** SM6375 uses the same
   mainline SDE display engine as SDM845/beryllium, so the `beryllium-*`
   display/scanout patches are an *adapt* (register offsets, panel timing), not
   a rewrite.
2. **WLAN/BT is WCN3990 — identical to beryllium.** Those patches port nearly
   as-is.
3. **Touch is Novatek NT36672C**, the same family as beryllium's NT36672A
   (but on SPI here, vs. bit-banged I²C on beryllium).

---

## 3. What is reusable vs. new (mapped to the tree)

| Layer | Verdict |
|---|---|
| `tools/renegade/` (PAC strip, root build, HFS+/sparse tools) | **Reusable unchanged** |
| `tools/mkpayload.py`, `tools/adt.py`, `tools/remotezip.py` | Reusable |
| `apps/Bergamota/` (SwiftUI control plane) + `scripts/bergamota` | Reusable (phone-agnostic; just feed it new boot-image paths) |
| `tools/bergamota-flash.py`, `tools/unpack_bootv2.py` | Reusable |
| `enxerto/` core (loader, inflate, MMU, fdt, fastboot, sparse, blake2b, bootui) | Reusable |
| `enxerto/` board layer (`board.h`, `ufs.c`, `usb_dwc3.c`, `pmic.c`, `charge*.c`, `storage.c`) | **New SM6375 values / drivers** |
| `xnu/patches-11215/` generic `sweet-*` patches (0003–0065, 0131–0172, iOS18 port) | **Mostly reusable** |
| `xnu/patches-11215/` `beryllium-*` patches (~90 files: display, touch, battery, charger, WLAN/BT, GPU, UFS) | **Rewritten for SM6375** |
| `tools/mkadt.py` | Needs a new `sm6375` board entry + per-board GIC/UFS/timer refactor |
| GPU kernel patches (0103, 0180–0189, 0214, 0232…) | **Rewritten for Adreno 619** |
| Display/scanout patches (0096, 0126, 0211, 0216, 0221, 0235, 0240–0247) | **Rewritten for DPU** (SDM845 uses MDP) |

The cleanest statement of the port: **~50% of the repo (Renegade, the generic
Heartwood patches, Enxerto's framework) carries over; ~50% (the board-specific
half) is new.**

---

## 4. Prerequisites

- **Toolchain environment.** The user is on Windows. What's actually needed:
  - Kernel builds run on **GitHub Actions macOS runners** (`xnu.yml`) — no
    local Mac required.
  - Renegade root/update runs on **Linux** — WSL2 covers it
    (`sudo apt install python3 curl hfsprogs util-linux zstd dropbear-bin …`).
  - The macOS control-plane app is optional for flashing (fastboot +
    `bergamota-flash.py` suffice), but a Mac is needed to *build* the on-device
    tools (`tools/sweetnet/`) — or just use the prebuilt `berga-tools.zip`
    from the releases page.
- **Device access** (already satisfied): bootloader **unlocked**, `fastboot`
  working, and a way to root or temp-boot a recovery for `adb pull
  /sys/firmware/fdt`. Wiping userdata is fine.
- **Apple artifacts** (kept out of git): the iOS 18.7.8 IPSW, and the
  DeviceTree from an iPhone X IPSW (`D22AP`) for the `/product` identity.
- **~20 GB** free for the Renegade root build.

---

## 5. Milestones

### M0 — Capture the stock device tree (day one)

This is the foundation: `board.h`, `mkadt.py`, and every `beryllium-*` patch
are keyed off the device tree.

1. Unlock is already done. From ABL fastboot, temp-boot a recovery
   (OrangeFox/TWRP for the Nord) and pull:

   ```
   adb pull /dev/block/by-name/boot backup/boot.img
   adb pull /sys/firmware/fdt dtb/sm6375/live.dtb
   ```

2. Extract and select the right DTB (`tools/extract-dtb.py` already handles
   header v0–v2; add v3 if the Nord's boot.img uses it):

   ```
   python3 tools/extract-dtb.py backup/boot.img dtb/sm6375 --live dtb/sm6375/live.dtb
   # -> dtb/sm6375/dtb
   ```

3. Record the SM6375 numbers into this plan (the "TBD from DT" list in §2):
   `qcom,msm-id`, `qcom,board-id`, GICD/GICR, UFS base, cont-splash base, IMEM
   restart-reason, CNTFRQ, RAM banks, XPU carve-outs, panel/touch/PMIC/WCN
   compatibles, boot.img header version, A/B or not.

**Exit:** `dtb/sm6375/dtb` on disk and a filled-in hardware-facts table.

### M1 — Enxerto board support (the bootloader)

**Status: plumbing done (2026-10-30); board drivers next.**

Done and preprocessor-verified (board values confirmed against the live DT):

1. `enxerto/board.h` — added `BOARD_SM6375` with the real SM6375 addresses
   (`FB_BASE 0x85200000`, `IMEM 0xc12565c`, `PSHOLD 0x440b000`, `GICD 0xf200000`,
   `GICR 0xf240000`, `UFS 0x04804000`). GIC/UFS/PSHOLD moved from shared
   constants to per-board (sweet/beryllium values unchanged).
2. `enxerto/build.sh` — added the `sm6375)` board case (boot.img **header v3**,
   Image.gz-dtb with the merged live DTB), the host-test board define, and the
   Bergamota fastboot image. TODO(device): verify the blair ABL gunzips a v3
   kernel and finds the DTB after the stream.
3. `enxerto/bootui.c` — `DEVICE_NAME "NORD CE 3 LITE"`.
4. `enxerto/enxerto.c` — the SDM845-only steps (charger init, charging mode,
   USB power-up, Bergamota fastboot, UFS probe) are now `#ifndef BOARD_SM6375`,
   so a SM6375 build skips them (they read SDM845 addresses) but still reaches
   the framebuffer console, `fdt_board_info` and the GIC/timer test (which now
   use correct board.h addresses).
5. `enxerto/fastboot.c` — product string via `-DFB_PRODUCT=\"sm6375\"`.

**✅ M1 validated on hardware (2026-10-30).** Flashed Enxerto to `boot_a` and
booted it. RAM boot (`fastboot boot`) is blocked on this OnePlus bootloader
(`remote: unknown command`), so flash is the only route. The panel console came
up and the milestone-2 GIC/timer diagnostic passed clean:

```
ENXERTO sm6375 GIC/timer test
EL cur=00000001 cntfrq=0124F800        # EL1, 19.2 MHz
MPIDR =81000000                        # same 0x81000n00 pattern as beryllium
GICD was=00000010 now=00000012         # 0xf200000 OK (EnableGrp1A stuck)
GICR base=0F240000 waker=00000000      # 0xf240000 OK
VTIMER irqs=0000001E other=00000000    # 30 IRQs, INTID 19, ZERO spurious
PTIMER irqs=0000001E other=00000000    # 30 IRQs, INTID 18
SGI1   irqs=0000000A other=00000000    # 10 SGIs
```

So the board constants in `board.h` are **confirmed correct on hardware**:
`FB_BASE 0x85200000`, `GICD 0xf200000`, `GICR 0xf240000`, timer 19.2 MHz /
INTID 19/18, and the SMP MPIDR layout is identical to beryllium (de-risks M2).

Format correction along the way: the Nord is a GKI/virtual-A/B device — the
kernel is a **raw Linux arm64 Image** (no gzip, no appended DTB; the ABL
supplies the DTB from `vendor_boot` + `dtbo`). The first build (gzip + appended
DTB, the beryllium layout) was wrong; `build.sh` now packs raw `out/Image`.

Not yet reachable remotely: USB is still guarded out, so the console is
panel-only for now. Restoring LineageOS = bootloader → `set_active b` → reboot
(`boot_a`, `boot_b`, `vendor_boot_a`, `dtbo_a` are backed up in `backup/`).

**Next (board drivers, needs the Linux blair register maps):**

- `usb_power.c` / `usb_dwc3.c`: **ported (2026-10-30).** SM6375 values from
  `gcc-sm6375.c` + `dwc3-qcom.c` + `phy-qcom-snps-femto-v2.c` (fetched into
  `research/linux-blair/`): GCC at **0x1400000**, USB GDSC `0x1a004`, DWC3 core
  at **0x4e00000**, QSCRATCH at **0x4ef8800** (base + 0xf8800, fixed), HS PHY
  `snps-femto-v2` at **0x162b000** (replaces the SDM845 qusb2), USB clock CBCR
  set {master 0x1a010, sleep 0x1a014, utmi 0x1a018, cfg-noc 0x1a084, sys-noc
  0x1a080, ahb2phy 0x1d008, clkref 0x8c010}, resets {prim 0x1a000, qusb2phy
  0x1c000, ahb2phy 0x1d000}. Master RCG is the same `src=1, hid_div=17`
  (GPLL0 = 600 MHz on both). The snps-femto init uses the full
  `qcom_snps_hsphy_init` sequence + the DT `qcom,param-override-seq`
  `<0xe7 0x6c 0x2c 0x70 0x3 0x74 0x3 0x78>`. SMMU is **qsmmu-v500** (different
  MMU) — defaulted off (`bergamota.smmu=1` opts in), since the DT carries
  `qcom,skip-init` (ABL leaves the USB stream configured). All board-conditional
  via `#ifdef BOARD_SM6375`; beryllium/sweet compile unchanged.
  **✅ Validated on hardware (2026-10-30):** the device enumerates over USB as
  `bergamota0001` (`product: sm6375`, `version-bootloader: bergamota-2`), so the
  snps-femto PHY init, the blair GCC clock bring-up and the DWC3 controller all
  work. The USB log/fastboot path is now live on the Nord.
- `ufs.c`: UFS 3.1 host at **0x04804000** (not ported yet).
- `pmic.c` / `charger.c` / `charge.c`: **PM6125 + PMK8350** on SPMI at
  **0x1c40000** (`qcom,spmi@1c40000`), replacing PMI8998/PM8998 (SPMI 0x0c440000).

### M2 — Heartwood boots to a console

**Status: `mkadt.py` SM6375 board entry done (2026-10-30); kernel build next.**

Refactored `tools/mkadt.py` so GIC (`gicd`/`gicr` + sizes) and UFS moved from
shared module constants into the `BOARDS` dict, then added the `sm6375` entry
(validated: `mkadt.py --board sm6375` produces the correct ADT):

- `dram-base 0xa0000000` / `dram-size 0x1b000000` (432 MiB, the clean window
  between the `cdsp@0x9b800000` carve-out end 0x9d600000, `kboot@0x9fe00000`,
  and `removed_region@0xc0000000`)
- `compat ['qcom,blair','qcom,blair-qrd','qcom,qrd','xiaomi,sweet']` (sweet kept
  for SweetPlatformExpert matching)
- CPU `qcom,kryo` 6×1.8GHz + 2×2.2GHz (`big_from=6`)
- `gicd 0xf200000` / `gicr 0xf240000`, `ufs 0x04804000`, `fb 0x85200000`
- WCN3990 Wi-Fi/BT synthetic MACs (as beryllium)

beryllium/sweet/qemu builds unchanged (regression-checked). The Apple
`d22ap.adt` /product node is still git-ignored (needs the iPhone X IPSW).

**Kernel build requires GitHub Actions** (`xnu.yml` on macOS runners) — no
local Windows build. `gh` + fork set up (2026-10-30): fork
`madcracker-git/Bergamota`, branch `sm6375-port` pushed, first baseline CI
build dispatched (run 40987011382, the full default patch set).

### M2a — Patch-set curation (the SM6375 kernel patch list)

The 219 files in `xnu/patches-11215/` split by name prefix: **121 `sweet-*`,
89 `beryllium-*`, 6 `ios18-*`, 5 `topaz-*`**, plus 3 `.sh` stage scripts.
The SM6375 kernel = the SoC-agnostic subset + new `sm6375-*` drivers. This is
the concrete carry/drop/replace list:

**Carry over unchanged (SoC-agnostic XNU port):**
- `0003`–`0071` `sweet-*` bring-up: framebuffer serial console, machine config,
  physmap padding, kmem RNG, device-tree platform expert, virtual-timer IRQ,
  boot CPU, libpthread/bearssl/hfs staging, mockfs ramdisk, AMFI, HFS, root/UFS
  bootstrap (see below), SMP/PSCI.
- `0102` `sweet-review-fixes`, `0131`–`0160` (`ios18-*` + `sweet-*`) iOS 18
  port: arm64e exec, dyld pager, IOSurface limits/layout, IOMFB swap record.
- `0141` FJCVTZS emulation, `0170`–`0173` xnu-11215 API fixes, `0195` early
  console, `0222` UFS durability syncer.
- The 3 `.sh` stage scripts (libpthread, bearssl, hfs).

**Already portable (ADT-driven, no code change):**
- `0028`/`0030` UFS root + writes: `sweet_ufs.c` locates its registers via
  `SecureDTLookupEntry("/sweet-ufs")` → the ADT `reg` I made per-board, so it
  reads `0x04804000` automatically. UFS 2.1 vs 3.1 register differences still
  need a boot test.

**Drop (SDM845/beryllium-specific) — write `sm6375-*` equivalents:**
- Display/scanout: `0096`, `0126`, `0128`–`0129`, `0210`–`0213`, `0215`–`0216`,
  `0220`–`0221`, `0224`, `0227`–`0249` (MDP → **SDE**; `qcom,sde-kms`).
- Touch/keys: `0060`, `0062`–`0063`, `0082`, `0085`–`0086`, `0101`, `0111`–
  `0114`, `0161` (NT36672C on SPI).
- USB kernel driver: `0072`–`0077`, `0081`, `0084`, `0087`, `0089`, `0094`,
  `0098`, `0107` — `sweet_usb.c` **hardcodes** `DWC3 0x0a600000`, `QUSB2
  0x088e2000`, `GCC 0x00100000` (SDM845); needs the SM6375 values from
  `enxerto/usb_dwc3.c`/`usb_power.c` (already ported).
- PMIC/charger/battery: `0092`, `0095`, `0130`, `0200`–`0201`, `0219`,
  `0223`, `0225`–`0226`, `0242`–`0249` (PMI8998/PM8998 → **PM6125/PMK8350**).
- WLAN/BT: `0104`, `0117`–`0136`, `0239` (WCN3990 — same chip, so these port
  nearly as-is; slim@a5c0000 vs beryllium's bus).
- GPU A630: `0103`, `0180`–`0189`, `0214`, `0232`, `0234`, `0236`–`0237`,
  `0251` (→ Adreno 619).
- Reboot/IMEM: `0137`, `0140` (IMEM base `0xc12565c` already in `board.h`).

**Note:** the `beryllium-*` count (89) is the *lower bound* on rewrite work;
several `sweet-*` drivers bake in SDM845 addresses (USB is the big one).

1. First boot: the generic series + the ADT, `sweet-*` only, see how far the
   early-framebuffer-debug chain (`0003`–`0012`) gets on the Nord.
2. Then add `sm6375-*` patches one driver at a time (UFS first — already
   ADT-driven — then display, then USB, then the rest).

**Exit:** the kernel prints its boot log to the panel/serial and reaches
`launchd` (or at least a stable console with a panic we can iterate on).

### M3 — Renegade root (mostly a formality)

1. Build the iOS 18.7.8 root exactly as today (`tools/renegade/renegade
   fetch-tools fetch root`). This step is SoC-independent.
2. The `mkboot-full.sh` path keys off `BOARD=beryllium`; add a `BOARD=sm6375`
   variant so it packs the SM6375 ADT + kernel + boot-args.

**Exit:** `ios18-root-full.simg` built and a boot image that mounts it.

### M4 — SpringBoard (CPU-rendered)

Iterate the iOS18-port patches (`0131`–`0160`, the arm64e/exec/dyld/SpringBoard
fixes) on top of M2. Most are SoC-agnostic. The board-specific gaps are the
panel timing, touch (needed for interaction but not for a screenshot), and the
fake `/product` identity already handled by `mkadt.py`.

**Exit:** SpringBoard on the home screen, SSH at `root@192.168.7.2` over the
CDC-ECM USB link.

### M5 — Adreno 619 (stretch, optional)

The kernel-side A630 patches (`0103`, `0180`–`0189`, `0214`, `0232`) become
a619 equivalents (new GMU/zap firmware, new power rails, new SMMU config). The
userspace side (Chlorophyll/Turnip, `gpu/metal` submodule) already targets a6xx
and is the more portable half. This is the largest remaining unknown and is
deliberately last.

---

## 6. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| DPU vs MDP display rewrite is the largest unknown | High | Land M1 console first; treat display scanout as its own workstream (`docs/DISPLAY_HW.md`) |
| SM6375 GIC/UFS/timer addresses differ from the shared constants in `mkadt.py` and the `sweet-*` patches | High | Refactor `mkadt.py` first (mechanical); confirm every address from the M0 DT |
| Adreno 619 GMU/zap firmware bring-up | High | Defer to M5; Turnip a6xx support lowers the userspace risk |
| Boot.img header version / ABL quirks on this specific device | Medium | Inspect the stock `boot.img` in M0; add v3 support to `extract-dtb.py` if needed |
| No Mac for `tools/sweetnet` builds | Medium | Use prebuilt `berga-tools.zip` from releases |
| Driver bugs only visible on hardware (SMMU faults etc.) | Medium | Reuse the `sweetgpu`/recovery diagnostics; keep the Bergamota USB log |

---

## 7. Immediate next actions

1. ~~**M0 now**: pull `boot.img` + `live.dtb` from the Nord, run
   `extract-dtb.py`, and fill in the §2 TBD table.~~ **Done.** `dtb/sm6375/live.dtb`
   + `live.dump.txt` captured from the device; §2 filled with the real values.
2. **M1 plumbing**: board.h / build.sh / bootui.c / enxerto.c guards — **done
   (2026-10-30)**. The boot image is not yet buildable here (Windows; needs a
   Linux/macOS toolchain + `aarch64-elf-gcc` + `mkbootimg`, e.g. WSL2).
3. **M1 board drivers**: port `usb_power.c`/`usb_dwc3.c`/`ufs.c`/`pmic.c`/
   `charger.c`/`charge.c` using the Linux `gcc-blair.c` + PM6125/PMK8350 maps
   (addresses already recorded in the M1 section above).
4. **First on-device test** (after a Linux/macOS build): `fastboot boot
   enxerto/out/enxerto-boot.img` and confirm the panel lights and shows the
   Bergamota console + `fdt_board_info` (blair facts).
5. **M2 prep**: refactor `tools/mkadt.py` to move GIC/UFS/timer into `BOARDS`.
