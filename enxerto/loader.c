/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 *
 * The copy/inflate runs with the MMU and D-cache on (payload, kernel
 * destination and our image write-back, everything else Device as before):
 * the bit-at-a-time inflater was ~10 MiB/min on uncached Device memory.
 * mmu_off_jump cleans it all to memory before xnu starts.
 */
#include <stdint.h>
#include "bootui.h"
#include "charger.h"
#include "console.h"
#include "fdt.h"
#include "inflate.h"
#include "loader.h"
#include "mmu.h"
#include "ufs.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

/*
 * version 1: 4 u64 per segment. version 2 (mkpayload.py --compress): a fifth u64, raw_size; when it is not 0 the
 * segment is raw DEFLATE data of file_size bytes that inflates to raw_size bytes (docs/IOS18_PORT.md section 11:
 * ABL places the boot.img ramdisk just below 0x85600000, so the payload must stay small).
 */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
	uint64_t raw_size;	/* version 2 only */
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	uint8_t segs[];		/* nsegs x (32 bytes v1, 40 bytes v2) */
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(sizeof(struct payload_seg) == 40, "payload segment layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

/* Diagnostics for the fastboot getvar bergamota-initrd. */
uint64_t loader_rd_start, loader_rd_end;
int64_t loader_magic_off;

/* Byte-wise reads for the payload header: the GKI ABL concatenates the vendor
 * ramdisk + boot ramdisk, so the payload can land at a 4-mod-8 (or any) offset
 * in the initrd; with the MMU off (Device memory) an unaligned 64-bit load
 * faults, but byte accesses are always safe. */
static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/*
 * Scan [start, end) for the SWEETXNU payload at 4-byte granularity (the vendor
 * ramdisk size is a multiple of 4, so a concatenated payload is 4-aligned but
 * not necessarily 8-aligned). The ABL on GKI devices concatenates the boot
 * ramdisk with the vendor ramdisk in either order, so the payload is not
 * necessarily at the start of the initrd.
 */
static int64_t loader_find_magic(uintptr_t start, uintptr_t end)
{
	for (uintptr_t p = start & ~3UL; p + 8 <= end; p += 4)
		if (magic_ok((const char *)p))
			return (int64_t)(p - start);
	return -1;
}

/* Boot screen progress: INFLATE spans permille 100..900 over all segments' output bytes. */
#define PM_INITRD	40
#define PM_PAYLOAD	70
#define PM_MMU		100
#define PM_INFLATE_END	900
static uint64_t infl_base, infl_total;

static void inflate_progress(uint64_t done)
{
	bootui_mib("INFLATE", (infl_base + done) >> 20, infl_total >> 20, PM_MMU, PM_INFLATE_END);
}

/*
 * Side-effect-free validation of a payload at [rd_start, rd_end): header sane, every
 * file range inside the payload, every destination inside the payload's own RAM window.
 * Payloads from the ABL ramdisk were trusted blindly; payloads that arrive over the
 * Bergamota fastboot link must not be able to write anywhere else.
 */
const char *loader_check(uintptr_t rd_start, uintptr_t rd_end)
{
	const uint8_t *h = (const uint8_t *)rd_start;
	uint64_t size = rd_end - rd_start;

	if (rd_end < rd_start || size < sizeof(struct payload))
		return "payload too short";
	if (!magic_ok((const char *)h))
		return "no SWEETXNU payload";
	uint32_t version = rd_u32(h + 8), nsegs = rd_u32(h + 12);
	uint64_t adt_off = rd_u64(h + 0x20), adt_size = rd_u64(h + 0x28);
	uint64_t virt_base = rd_u64(h + 0x10), kernel_end = rd_u64(h + 0x30);
	uint64_t phys_base = rd_u64(h + 0x38), mem_size = rd_u64(h + 0x40);
	if ((version != 1 && version != 2) || (adt_off & 7) || (rd_start & 3))
		return "bad payload header";
	const unsigned stride = version == 2 ? 40 : 32;
	if (nsegs == 0 || nsegs > 64 || sizeof(struct payload) + (uint64_t)nsegs * stride > size)
		return "bad segment table";
	uint64_t ram_lo = phys_base, ram_hi = phys_base + mem_size;
	if (ram_hi <= ram_lo)
		return "bad RAM window";
	if (rd_end > ram_lo && rd_start < ram_hi)
		return "ramdisk overlaps kernel memory";
	if (adt_off > size || adt_size > size - adt_off)
		return "ADT outside payload";
	uint64_t pa_of_va_base = phys_base - virt_base;
	uint64_t kend_pa = kernel_end + pa_of_va_base;
	if (kend_pa < ram_lo || kend_pa >= ram_hi || adt_size > ram_hi - kend_pa ||
	    ram_hi - kend_pa - adt_size < 2 * PAGE_16K)
		return "kernel end outside RAM";
	for (uint32_t i = 0; i < nsegs; i++) {
		const uint8_t *s = h + sizeof(struct payload) + (uint64_t)i * stride;
		uint64_t vmaddr = rd_u64(s), vmsize = rd_u64(s + 8);
		uint64_t file_off = rd_u64(s + 16), file_size = rd_u64(s + 24);
		uint64_t raw_size = version == 2 ? rd_u64(s + 32) : 0;
		uint64_t pa = vmaddr + pa_of_va_base;
		if (file_off > size || file_size > size - file_off)
			return "segment outside payload";
		if (pa < ram_lo || pa > kend_pa || vmsize > kend_pa - pa)
			return "segment outside RAM";
		if (version == 2 && raw_size) {
			if (raw_size > vmsize)
				return "segment inflates past its size";
		} else if (file_size > vmsize) {
			return "segment larger than its size";
		}
	}
	return 0;
}

int loader_has_payload(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end) || rd_end < rd_start + 8)
		return 0;
	loader_rd_start = rd_start;
	loader_rd_end = rd_end;
	loader_magic_off = loader_find_magic(rd_start, rd_end);
	return loader_magic_off >= 0;
}

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	loader_rd_start = rd_start;
	loader_rd_end = rd_end;
	loader_magic_off = loader_find_magic(rd_start, rd_end);
	if (loader_magic_off < 0)
		return "no SWEETXNU payload in initrd";
	return loader_boot_payload(rd_start + (uintptr_t)loader_magic_off, rd_end, 0);
}

/*
 * Boot the payload in [rd_start, rd_end) (ABL ramdisk or the fastboot download); returns only on error.
 * verify: run loader_check() first (always for downloads; the ABL ramdisk path keeps its old trust).
 */
const char *loader_boot_payload(uintptr_t rd_start, uintptr_t rd_end, int verify)
{
	const uint8_t *h = (const uint8_t *)rd_start;
	if (rd_end < rd_start + 8 || !magic_ok((const char *)h))
		return "no SWEETXNU payload";
	/* A real payload: switch to the branded boot screen (con_* output becomes its log window). */
	bootui_init();
	bootui_progress(PM_INITRD, "INITRD FOUND");
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
	if (verify) {
		const char *bad = loader_check(rd_start, rd_end);
		if (bad)
			return bad;
	}
	uint32_t version = rd_u32(h + 8), nsegs = rd_u32(h + 12);
	uint64_t virt_base = rd_u64(h + 0x10), entry_va = rd_u64(h + 0x18);
	uint64_t adt_off = rd_u64(h + 0x20), adt_size = rd_u64(h + 0x28);
	uint64_t kernel_end = rd_u64(h + 0x30);
	uint64_t phys_base = rd_u64(h + 0x38), mem_size = rd_u64(h + 0x40);
	if ((version != 1 && version != 2) || (adt_off & 7) || (rd_start & 3))
		return "bad payload header";
	const unsigned stride = version == 2 ? 40 : 32;

	uint64_t pa_of_va_base = phys_base - virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", phys_base, "size=", mem_size);
	con_hex("VIRT", virt_base);
	con_hex("ENTRY", entry_va);
	bootui_progress(PM_PAYLOAD, "PAYLOAD OK");

	/* Kernel destination through boot_args (see below), and the payload: cacheable while we work. */
	uint64_t kend_pa = kernel_end + pa_of_va_base;
	uint64_t top_pa = ((kend_pa + adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1)) + PAGE_16K;

	/* The initrd must not overlap the kernel IMAGE destination [phys_base, top_pa).
	 * (The kernel's whole RAM window is much bigger; the initrd sitting inside it is
	 * normal on SM6375 — the ABL places it there — and harmless, because the loader
	 * has finished reading by the time the kernel reuses that RAM.) */
	if (rd_end > phys_base && rd_start < top_pa)
		return "ramdisk overlaps kernel image";

	struct mmu_range wb[3] = {
		{ phys_base, top_pa },
		{ rd_start, rd_end },
		{ 0, 0 },	/* the image: filled in from mmu_info */
	};
	struct mmu_info mi;
	int mmu_rc = mmu_enable_ranges(&mi, wb, 2);
	con_hex2("MMU", "rc=", (uint32_t)mmu_rc, "l3=", (uint32_t)mi.n_l3);
	wb[2] = (struct mmu_range){ mi.img_lo, mi.img_hi };
	bootui_progress(PM_MMU, "MMU ON");

	/* From here the initrd is Normal write-back (mmu_enable_ranges), so unaligned
	 * 8-byte reads of the segment table are fine; use byte reads anyway for clarity. */
	infl_base = 0;
	infl_total = 0;
	for (uint32_t i = 0; i < nsegs; i++) {
		const uint8_t *s = h + sizeof(struct payload) + (uint64_t)i * stride;
		uint64_t raw_size = version == 2 ? rd_u64(s + 32) : 0;
		infl_total += (version == 2 && raw_size) ? raw_size : rd_u64(s + 24);
	}

	for (uint32_t i = 0; i < nsegs; i++) {
		const uint8_t *s = h + sizeof(struct payload) + (uint64_t)i * stride;
		uint64_t vmaddr = rd_u64(s), vmsize = rd_u64(s + 8);
		uint64_t file_off = rd_u64(s + 16), file_size = rd_u64(s + 24);
		uint64_t raw_size = version == 2 ? rd_u64(s + 32) : 0;
		uintptr_t dst = vmaddr + pa_of_va_base;
		uint64_t raw = file_size;
		if (version == 2 && raw_size) {
			uint64_t cap = vmsize;
			con_hex2("INFLATE", "in=", (uint32_t)file_size, "out=", (uint32_t)raw_size);
			int rc = inflate_raw((const uint8_t *)(rd_start + file_off), file_size,
					     (volatile uint8_t *)dst, &cap, inflate_progress);
			if (rc || cap != raw_size) {
				con_hex2("INFLATE", "rc=", (uint32_t)rc, "got=", (uint32_t)cap);
				return "inflate failed";
			}
			raw = cap;
		} else {
			copy64(dst, rd_start + file_off, file_size);
		}
		infl_base += raw;
		bootui_mib("INFLATE", infl_base >> 20, infl_total >> 20, PM_MMU, PM_INFLATE_END);
		if (vmsize > raw)
			zero64(dst + ((raw + 7) & ~7UL), vmsize - ((raw + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", nsegs, "end=", kernel_end + pa_of_va_base);
	bootui_progress(920, "SEGMENTS READY");

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + adt_off, adt_size);
	uintptr_t ba_pa = (adt_pa + adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
	if (top != top_pa)
		return "boot_args placement mismatch";

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = h[0x48 + i];
	ba->memSizeActual = mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);
	bootui_progress(960, "BOOT ARGS");

	uintptr_t entry_pa = entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	bootui_progress(1000, "STARTING IOS");
	enxerto_charger_handoff();	/* the kernel's sweetcharger=2 finds the charger registers as ABL left them */
#ifdef BOARD_SM6375
	/* Bring the UFS host up (full Qualcomm sequence + HS gear-up) and leave it
	 * running, so the kernel's sweet_ufs.c finds it ready to take over. SM6375's
	 * ABL does NOT leave UFS up for a bare-metal taker (HCE clear, host error). */
	{
		int urc = ufs_init();
		con_hex2("UFS", "init=", (uint32_t)urc, "stage=", ufs_last.stage);
	}
#endif
	con_str("XNU", "jumping (fbcon clears screen)");
	bootui_handoff();

	/* Clean to memory, MMU and caches off, invalidate, jump (also right when mmu_rc failed). */
#ifdef BOOT_PAUSE_MS
	/* debug builds (BOOTPAUSE=ms build.sh): leave the log on screen long enough for a photo */
	{
		uint64_t f, t0, t;
		__asm__ volatile("mrs %0, cntfrq_el0; mrs %1, cntvct_el0" : "=r"(f), "=r"(t0));
		do
			__asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
		while (t - t0 < f / 1000 * BOOT_PAUSE_MS);
	}
#endif
	mmu_off_jump(entry_pa, ba_pa, wb);
}
