// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <dm.h>
#include <dm/uclass-internal.h>
#include <command.h>
#include <efi_loader.h>
#include <env.h>
#include <fs.h>
#include <u-boot/uuid.h>
#include <lmb.h>
#include <nvme.h>
#include <part.h>
#include <time.h>
#include <linux/ioport.h>
#include <linux/delay.h>

#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/arch/rtkit.h>
#include <asm/arch/j713_fdt.h>
#include <asm/arch/nvme_apple.h>

DECLARE_GLOBAL_DATA_PTR;

/* Apple M1/M2 */

static struct mm_region t8103_mem_map[] = {
	{
		/* I/O */
		.virt = 0x200000000,
		.phys = 0x200000000,
		.size = 2UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x500000000,
		.phys = 0x500000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x680000000,
		.phys = 0x680000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x6a0000000,
		.phys = 0x6a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x6c0000000,
		.phys = 0x6c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x800000000,
		.phys = 0x800000000,
		.size = 8UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M1 Pro/Max */

static struct mm_region t6000_mem_map[] = {
	{
		/* I/O */
		.virt = 0x280000000,
		.phys = 0x280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xf00000000,
		.phys = 0xf00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x1300000000,
		.phys = 0x1300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M1 Ultra */

static struct mm_region t6002_mem_map[] = {
	{
		/* I/O */
		.virt = 0x280000000,
		.phys = 0x280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xf00000000,
		.phys = 0xf00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x1300000000,
		.phys = 0x1300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2280000000,
		.phys = 0x2280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2380000000,
		.phys = 0x2380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2580000000,
		.phys = 0x2580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x25a0000000,
		.phys = 0x25a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x25c0000000,
		.phys = 0x25c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2700000000,
		.phys = 0x2700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2b00000000,
		.phys = 0x2b00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2f00000000,
		.phys = 0x2f00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x3300000000,
		.phys = 0x3300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M2 Pro/Max */

static struct mm_region t6020_mem_map[] = {
	{
		/* I/O */
		.virt = 0x280000000,
		.phys = 0x280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x340000000,
		.phys = 0x340000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x400000000,
		.phys = 0x400000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x480000000,
		.phys = 0x480000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xf00000000,
		.phys = 0xf00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x1300000000,
		.phys = 0x1300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M2 Ultra */

static struct mm_region t6022_mem_map[] = {
	{
		/* I/O */
		.virt = 0x280000000,
		.phys = 0x280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x340000000,
		.phys = 0x340000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x400000000,
		.phys = 0x400000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x480000000,
		.phys = 0x480000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0xf00000000,
		.phys = 0xf00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x1300000000,
		.phys = 0x1300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2280000000,
		.phys = 0x2280000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2340000000,
		.phys = 0x2340000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2380000000,
		.phys = 0x2380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2400000000,
		.phys = 0x2400000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2480000000,
		.phys = 0x2480000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE |
			PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2580000000,
		.phys = 0x2580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x25a0000000,
		.phys = 0x25a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x25c0000000,
		.phys = 0x25c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2700000000,
		.phys = 0x2700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2b00000000,
		.phys = 0x2b00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x2f00000000,
		.phys = 0x2f00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x3300000000,
		.phys = 0x3300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M3 */

static struct mm_region t8122_mem_map[] = {
	{
		/* I/O */
		.virt = 0x200000000,
		.phys = 0x200000000,
		.size = 4UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* NVMe */
		.virt = 0x300000000,
		.phys = 0x300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC0 */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC1 */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 8UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M3 Pro and Max */

static struct mm_region t6030_mem_map[] = {
	{
		/* I/O */
		.virt = 0x280000000,
		.phys = 0x280000000,
		.size = 2UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O */
		.virt = 0x300000000,
		.phys = 0x300000000,
		.size = 3UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x580000000,
		.phys = 0x580000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5a0000000,
		.phys = 0x5a0000000,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE */
		.virt = 0x5c0000000,
		.phys = 0x5c0000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC0 */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC1 */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC2 */
		.virt = 0xf00000000,
		.phys = 0xf00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* I/O ATC3 */
		.virt = 0x1300000000,
		.phys = 0x1300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* RAM */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* Framebuffer */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

/* Apple M4 */

static struct mm_region t8132_mem_map[CONFIG_NR_DRAM_BANKS + 10] = {
	{
		/* SoC I/O, including PMGR, WD1, MTP, AIC and UART */
		.virt = 0x380000000,
		.phys = 0x380000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* USB DWC3, PHY and DART aperture */
		.virt = 0x400000000,
		.phys = 0x400000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* ANS/NVMe and NVMMU aperture */
		.virt = 0x480000000,
		.phys = 0x480000000,
		.size = SZ_2G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* J713 APCIE ECAM, from its runtime DT (config reg). */
		.virt = 0x1cb0000000,
		.phys = 0x1cb0000000,
		.size = SZ_256M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* J713 APCIE endpoint windows (both DT ranges). */
		.virt = 0xb80000000,
		.phys = 0xb80000000,
		.size = SZ_1G + SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* GPU and DCP register aperture. */
		.virt = 0x300000000,
		.phys = 0x300000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* ATC0 I/O */
		.virt = 0x700000000,
		.phys = 0x700000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* ATC1 I/O */
		.virt = 0xb00000000,
		.phys = 0xb00000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}
};

/* Apple A18 Pro (T8140) */

static struct mm_region t8140_mem_map[CONFIG_NR_DRAM_BANKS + 3] = {
	{
		/* I/O */
		.virt = 0x200000000,
		.phys = 0x200000000,
		.size = 12UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}
};

/*
 * Apple M4 Pro (T6040; its blocks carry T6041 compatibles).  Every register
 * block in the J616s devicetree below DRAM lies in 0x2_0000_0000 to
 * 0x6_0000_0000, apart from the APCIE ECAM and the PCIe windows.  The RAM
 * banks and the framebuffer are filled in from the m1n1-prepared FDT, as on
 * T8140.
 */
static struct mm_region t6040_mem_map[CONFIG_NR_DRAM_BANKS + 5] = {
	{
		/* I/O */
		.virt = 0x200000000,
		.phys = 0x200000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* APCIE ECAM (the DT's config reg) */
		.virt = 0x1cb0000000,
		.phys = 0x1cb0000000,
		.size = SZ_256M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* APCIE endpoint windows (both DT ranges) */
		.virt = 0xb80000000,
		.phys = 0xb80000000,
		.size = SZ_1G + SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}
};

/* Apple M5 (J813), primary I/O range measured from the live Apple ADT.
 * RAM and framebuffer entries are populated from the m1n1-patched FDT.
 */
static struct mm_region t8142_mem_map[] = {
	{
		.virt = 0x210000000,
		.phys = 0x210000000,
		.size = 0x3f0000000,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* APCIE ECAM (the DT's config reg) */
		.virt = 0x1cb0000000,
		.phys = 0x1cb0000000,
		.size = SZ_256M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* APCIE endpoint windows (both DT ranges) */
		.virt = 0xb80000000,
		.phys = 0xb80000000,
		.size = SZ_1G + SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_INNER_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = 16UL * SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE
	}, {
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		0,
	}
};

/* T8152: only DockChannel is qualified for initial RAM boot. */
static struct mm_region t8152_mem_map[] = {
	{
		.virt = 0x308000000,
		.phys = 0x308000000,
		.size = SZ_2M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* Replaced by the first FDT memory bank before enabling caches. */
		.virt = 0x10000000000,
		.phys = 0x10000000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE
	}, {
		/* Inherited framebuffer, populated from the FDT. */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		0,
	}
};

struct mm_region *mem_map;

int board_init(void)
{
	return 0;
}

/*
 * dram_init() and dram_init_banksize() run before relocation with the data
 * cache off, where a walk of the flattened device tree is slow: about 60 ms
 * on the J700, whose 128 KiB tree has the memory node near its end.
 * ofnode_read_resource() walks the tree up to the node twice for every reg
 * entry (parent lookup and address translation), which cost about 4 s for
 * the twelve J700 DRAM ranges. Walk the tree once in dram_init() instead
 * and keep the result for dram_init_banksize().
 *
 * The result is what fdtdec_setup_mem_size_base() and
 * fdtdec_setup_memory_banksize() give: the same banks in the same order
 * and the same errors. Before relocation the control tree is always flat.
 *
 * In .data because .bss is not usable before relocation.
 */
static struct {
	int ret;	/* for dram_init_banksize() */
	int ranges;	/* reg entries over all memory nodes */
	int banks;
	struct {
		phys_addr_t start;
		phys_size_t size;
	} bank[CONFIG_NR_DRAM_BANKS];
} apple_dram __section(".data");

/*
 * Entry @index of the reg property of @node, as ofnode_read_resource()
 * returns it. For a child of the root that is a plain decode: the cell
 * counts are the root's and the address translation is an identity.
 * Anything else takes the generic path.
 */
static int apple_dram_entry(const void *fdt, int node, int depth, int index,
			    phys_addr_t *start, phys_size_t *size)
{
	const fdt32_t *reg;
	struct resource res;
	int na, ns, len;

	na = fdt_address_cells(fdt, 0);
	ns = fdt_size_cells(fdt, 0);
	if (depth != 1 || na < 1 || na > 2 || ns < 1 || ns > 2) {
		if (ofnode_read_resource(offset_to_ofnode(node), index, &res))
			return -EINVAL;
		*start = res.start;
		*size = res.end - res.start + 1;
		return 0;
	}

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || index >= len / (int)sizeof(*reg) / (na + ns))
		return -EINVAL;
	reg += index * (na + ns);
	*start = fdt_read_number(reg, na);
	*size = fdt_read_number(reg + na, ns);

	return 0;
}

static bool apple_dram_is_memory(const void *fdt, int node)
{
	const char *type;
	int len;

	type = fdt_getprop(fdt, node, "device_type", &len);

	return type && len == sizeof("memory") &&
	       !memcmp(type, "memory", sizeof("memory")) &&
	       ofnode_is_enabled(offset_to_ofnode(node));
}

/* Matches what fdt_path_offset(fdt, "/memory") finds. */
static bool apple_dram_is_path(const void *fdt, int node)
{
	const char *name = fdt_get_name(fdt, node, NULL);

	return name && !strncmp(name, "memory", 6) &&
	       (!name[6] || name[6] == '@');
}

int dram_init(void)
{
	const void *fdt = gd->fdt_blob;
	phys_addr_t start;
	phys_size_t size;
	int node, depth = -1, path = -1, nodes = 0, i;
	bool fill = true;

	apple_dram.ret = -EINVAL;
	apple_dram.ranges = 0;
	apple_dram.banks = 0;

	for (node = fdt_next_node(fdt, -1, &depth); node >= 0 && depth >= 0;
	     node = fdt_next_node(fdt, node, &depth)) {
		if (path < 0 && depth == 1 && apple_dram_is_path(fdt, node))
			path = node;
		if (!apple_dram_is_memory(fdt, node))
			continue;

		/* fdtdec_setup_memory_banksize() needs the first node only. */
		if (!nodes++)
			apple_dram.ret = 0;
		for (i = 0; !apple_dram_entry(fdt, node, depth, i, &start, &size); i++) {
			apple_dram.ranges++;
			if (!fill || apple_dram.banks == CONFIG_NR_DRAM_BANKS)
				continue;
			apple_dram.bank[apple_dram.banks].start = start;
			apple_dram.bank[apple_dram.banks].size = size;
			apple_dram.banks++;
		}
		/* ... and stops at the first empty node after that one. */
		if (!i && nodes > 1)
			fill = false;
	}

	if (apple_dram.ranges > CONFIG_NR_DRAM_BANKS)
		apple_dram.ret = -E2BIG;

	if (path < 0 || apple_dram_entry(fdt, path, 1, 0, &start, &size)) {
		debug("%s: no /memory range\n", __func__);
		return -EINVAL;
	}
	gd->ram_base = (unsigned long)start;
	gd->ram_size = size;

	return 0;
}

int dram_init_banksize(void)
{
	int bank;

	/* fdtdec_setup_memory_banksize() would stop silently at the limit. */
	if (apple_dram.ret == -E2BIG)
		log_err("Apple: %d DRAM ranges exceed CONFIG_NR_DRAM_BANKS=%d\n",
			apple_dram.ranges, CONFIG_NR_DRAM_BANKS);
	if (apple_dram.ret)
		return apple_dram.ret;

	for (bank = 0; bank < apple_dram.banks; bank++) {
		gd->bd->bi_dram[bank].start = apple_dram.bank[bank].start;
		gd->bd->bi_dram[bank].size = apple_dram.bank[bank].size;
	}

	return 0;
}

extern long fw_dtb_pointer;

int board_fdt_blob_setup(void **fdtp)
{
	/* Return DTB pointer passed by m1n1 */
	*fdtp = (void *)fw_dtb_pointer;

	return 0;
}

void build_mem_map(void)
{
	ofnode node;
	fdt_addr_t base;
	fdt_size_t size;
	int i, bank, fb_slot;

	if (of_machine_is_compatible("apple,t8103") ||
	    of_machine_is_compatible("apple,t8112"))
		mem_map = t8103_mem_map;
	else if (of_machine_is_compatible("apple,t6000") ||
		 of_machine_is_compatible("apple,t6001"))
		mem_map = t6000_mem_map;
	else if (of_machine_is_compatible("apple,t6002"))
		mem_map = t6002_mem_map;
	else if (of_machine_is_compatible("apple,t6020") ||
		 of_machine_is_compatible("apple,t6021"))
		mem_map = t6020_mem_map;
	else if (of_machine_is_compatible("apple,t6022"))
		mem_map = t6022_mem_map;
	else if (of_machine_is_compatible("apple,t8122"))
		mem_map = t8122_mem_map;
	else if (of_machine_is_compatible("apple,t6030") ||
		 of_machine_is_compatible("apple,t6031") ||
		 of_machine_is_compatible("apple,t6034"))
		mem_map = t6030_mem_map;
	else if (of_machine_is_compatible("apple,t8132"))
		mem_map = t8132_mem_map;
	else if (of_machine_is_compatible("apple,t8140"))
		mem_map = t8140_mem_map;
	else if (of_machine_is_compatible("apple,t6040"))
		mem_map = t6040_mem_map;
	else if (of_machine_is_compatible("apple,t8142"))
		mem_map = t8142_mem_map;
	else if (of_machine_is_compatible("apple,t8152") &&
		 of_machine_is_compatible("apple,j873g"))
		mem_map = t8152_mem_map;
	else
		panic("Unsupported SoC\n");

	if (mem_map == t8140_mem_map || mem_map == t8132_mem_map ||
	    mem_map == t6040_mem_map) {
		int ram_slot = mem_map == t8132_mem_map ? 8 :
			       mem_map == t6040_mem_map ? 3 : 1;

		/* LMB and EFI see every bank, so the MMU must map every bank. */
		for (bank = 0; bank < CONFIG_NR_DRAM_BANKS; bank++) {
			base = gd->bd->bi_dram[bank].start;
			size = gd->bd->bi_dram[bank].size;
			if (!size)
				break;
			size += base - ALIGN_DOWN(base, SZ_4K);
			base = ALIGN_DOWN(base, SZ_4K);
			mem_map[bank + ram_slot].virt = base;
			mem_map[bank + ram_slot].phys = base;
			mem_map[bank + ram_slot].size = ALIGN(size, SZ_4K);
			mem_map[bank + ram_slot].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
						  PTE_BLOCK_INNER_SHARE;
		}
		fb_slot = bank + ram_slot;
		mem_map[fb_slot].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
					 PTE_BLOCK_INNER_SHARE |
					 PTE_BLOCK_PXN | PTE_BLOCK_UXN;
	} else {
		/* Find list terminator and update the original single RAM slot. */
		for (i = 0; mem_map[i].size || mem_map[i].attrs; i++)
			;
		base = gd->bd->bi_dram[0].start;
		size = gd->bd->bi_dram[0].size;
		size += base - ALIGN_DOWN(base, SZ_4K);
		base = ALIGN_DOWN(base, SZ_4K);
		mem_map[i - 2].virt = base;
		mem_map[i - 2].phys = base;
		mem_map[i - 2].size = ALIGN(size, SZ_4K);
		fb_slot = i - 1;
	}

	node = ofnode_path("/chosen/framebuffer");
	if (!ofnode_valid(node))
		return;

	base = ofnode_get_addr_size(node, "reg", &size);
	if (base == FDT_ADDR_T_NONE)
		return;

	/* Align framebuffer mapping to page boundaries */
	size += (base - ALIGN_DOWN(base, SZ_4K));
	base = ALIGN_DOWN(base, SZ_4K);
	size = ALIGN(size, SZ_4K);

	/* Add framebuffer mapping */
	mem_map[fb_slot].virt = base;
	mem_map[fb_slot].phys = base;
	mem_map[fb_slot].size = size;
}

void enable_caches(void)
{
	build_mem_map();

	icache_enable();
	dcache_enable();
}

u64 get_page_table_size(void)
{
	return SZ_256K;
}

/*
 * The ESP U-Boot boots from, as "dev:part".  It is the ESP that the boot
 * loader names in /chosen/asahi,efi-system-partition.  Where that ESP holds
 * no EFI boot loader but another ESP on the same disk does, the other one is
 * used: on a Mac whose stub ESP (where Stage 1 finds Stage 2) is not the
 * ESP of the installed OS, the loader's ESP is the OS's.  The OS is then
 * told that ESP (see ft_board_setup()), since its firmware and boot files
 * live there.
 */
#define ASAHI_EFI_LOADER	"EFI/BOOT/BOOTAA64.EFI"

static char asahi_esp_uuid[UUID_STR_LEN + 1];
static bool asahi_esp_moved;

static bool asahi_esp_has_loader(struct blk_desc *desc, int part)
{
	if (fs_set_blk_dev_with_part(desc, part))
		return false;

	return fs_exists(ASAHI_EFI_LOADER);
}

static char *asahi_esp_devpart(void)
{
	struct disk_partition info;
	struct blk_desc *nvme_blk;
	const char *uuid = NULL;
	int devnum, len, p, part, named, first, loader;
	bool named_loader;
	char named_uuid[UUID_STR_LEN + 1] = "";
	char loader_uuid[UUID_STR_LEN + 1] = "";
	char first_uuid[UUID_STR_LEN + 1] = "";
	static char devpart[64];
	struct udevice *dev;
	ofnode node;

	if (devpart[0])
		return devpart;

	/* Initial T8152 boot is RAM-only; do not probe its unported ANS. */
	if (of_machine_is_compatible("apple,t8152"))
		return devpart;

	node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		uuid = ofnode_get_property(node, "asahi,efi-system-partition",
					   &len);
	}

	nvme_scan_namespace();
	for (devnum = 0, part = 0; ; devnum++) {
		nvme_blk = blk_get_devnum_by_uclass_id(UCLASS_NVME, devnum);
		if (!nvme_blk)
			break;

		dev = dev_get_parent(nvme_blk->bdev);
		if (!device_is_compatible(dev, "apple,nvme-ans2") &&
		    !device_is_compatible(dev, "apple,t8132-nvme-ans2") &&
		    !device_is_compatible(dev, "apple,t6041-nvme-ans2"))
			continue;

		named = first = loader = 0;
		named_loader = false;
		for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
			if (part_get_info(nvme_blk, p, &info) < 0)
				break;
			if (!(info.bootable & PART_EFI_SYSTEM_PARTITION))
				continue;

			if (!first) {
				first = p;
				strlcpy(first_uuid, info.uuid, sizeof(first_uuid));
			}
			if (!named && uuid && !strcasecmp(uuid, info.uuid)) {
				named = p;
				strlcpy(named_uuid, info.uuid, sizeof(named_uuid));
				named_loader = asahi_esp_has_loader(nvme_blk, p);
				continue;
			}
			if (!loader && asahi_esp_has_loader(nvme_blk, p)) {
				loader = p;
				strlcpy(loader_uuid, info.uuid, sizeof(loader_uuid));
			}
		}

		if (named && (named_loader || !loader)) {
			part = named;
			strlcpy(asahi_esp_uuid, named_uuid, sizeof(asahi_esp_uuid));
		} else if (loader) {
			part = loader;
			strlcpy(asahi_esp_uuid, loader_uuid, sizeof(asahi_esp_uuid));
			asahi_esp_moved = true;
			debug("ESP: partition %d has the EFI boot loader; the %s ESP (%d) has none\n",
			      loader, named ? "named" : "first", named ?: first);
		} else if (first) {
			part = first;
			strlcpy(asahi_esp_uuid, first_uuid, sizeof(asahi_esp_uuid));
		}

		if (part > 0)
			break;
	}

	if (part > 0) {
		snprintf(devpart, sizeof(devpart), "%x:%x", devnum, part);
		efi_system_partition.uclass_id = UCLASS_NVME;
		efi_system_partition.devnum = devnum;
		efi_system_partition.part = part;
	}

	return devpart;
}

const char *env_fat_get_intf(void)
{
	return blk_get_uclass_name(UCLASS_NVME);
}

char *env_fat_get_dev_part(void)
{
	return asahi_esp_devpart();
}

#define KERNEL_COMP_SIZE	SZ_128M

#define lmb_alloc(size, addr) lmb_alloc_mem(LMB_MEM_ALLOC_ANY, SZ_2M, addr, size, LMB_NONE)

/*
 * Aurora Silicon boot flow for J700 (T8140), J713 (T8132) and J616s (T6040).
 *
 * m1n1 hands U-Boot the Linux devicetree it prepared, with the kernel
 * command line in /chosen/bootargs.  First load the removable-media EFI
 * path from the internal ESP, which selects the installed EFI boot manager.
 * If that fails, boot the kernel and initramfs named by /chosen fallback
 * properties, then try generic bootflow.  Both direct and EFI boot pass
 * m1n1's devicetree (${fdtcontroladdr}).  With no
 * aurora,fallback-initrd, an initramfs m1n1 already loaded is used.
 *
 * Nothing here is set if the variable already exists, so a saved
 * uboot.env on the ESP can override any of it.
 */
static const char * const apple_efi_env[][2] = {
	{ "aurora_boot",
	  "run aurora_efi; "
	  "echo AURORA_UBOOT_BOOT: fallback; run aurora_fallback; "
	  "echo AURORA_UBOOT_BOOT: bootflow; bootflow scan -b; "
	  "echo AURORA_UBOOT_BOOT_FAIL: no bootable entry; run aurora_rescue" },
	{ "aurora_efi",
	  "if test -n \"${fw_dev_part}\"; then "
	  "load nvme ${fw_dev_part} ${kernel_addr_r} EFI/BOOT/BOOTAA64.EFI && "
	  "bootefi ${kernel_addr_r} ${fdtcontroladdr}; fi" },
	/* Do not restart the HID transport from a failed EFI boot path. */
	{ "aurora_rescue",
	  "echo; echo Nothing could be booted. Use the serial console or reset." },
	{ "aurora_fallback",
	  "if test -n \"${aurora_fallback_image}\" -a -n \"${fw_dev_part}\"; then "
	  "load nvme ${fw_dev_part} ${kernel_addr_r} ${aurora_fallback_image} && "
	  "run aurora_fallback_initrd_load && "
	  "booti ${kernel_addr_r} ${aurora_initrd} ${fdtcontroladdr}; "
	  "fi" },
	/*
	 * Without a named initramfs, use the one the boot loader already
	 * placed in memory (m1n1 passes it in /chosen/linux,initrd-*).
	 */
	{ "aurora_fallback_initrd_load",
	  "if test -n \"${aurora_fallback_initrd}\"; then "
	  "load nvme ${fw_dev_part} ${ramdisk_addr_r} ${aurora_fallback_initrd} && "
	  "setenv aurora_initrd ${ramdisk_addr_r}:${filesize}; "
	  "elif test -n \"${aurora_loader_initrd}\"; then "
	  "setenv aurora_initrd ${aurora_loader_initrd}; "
	  "else setenv aurora_initrd -; fi" },
};

/* The initramfs the boot loader placed in memory, as "start:size" */
static void apple_loader_initrd(ofnode chosen)
{
	u64 start, end;
	char buf[40];

	if (ofnode_read_u64(chosen, "linux,initrd-start", &start) ||
	    ofnode_read_u64(chosen, "linux,initrd-end", &end) ||
	    end <= start)
		return;

	snprintf(buf, sizeof(buf), "%llx:%llx", start, end - start);
	env_set("aurora_loader_initrd", buf);
}

static void apple_efi_late_init(bool start_keyboard)
{
	const char *str;
	ofnode chosen;
	int i;

	for (i = 0; i < ARRAY_SIZE(apple_efi_env); i++) {
		if (!env_get(apple_efi_env[i][0]))
			env_set(apple_efi_env[i][0], apple_efi_env[i][1]);
	}

	/*
	 * Replace the generic Apple defaults unless a saved environment
	 * changed them.  USB is neither started at boot nor a boot target
	 * until the USB-C PHY support has been validated; "usb start" still
	 * works by hand.
	 */
	if (!strcmp(env_get("bootcmd") ?: "", CONFIG_BOOTCOMMAND))
		env_set("bootcmd", "run aurora_boot");
	if (!strcmp(env_get("boot_targets") ?: "", "nvme mmc usb"))
		env_set("boot_targets", "nvme mmc");
	if (!strcmp(env_get("preboot") ?: "", "usb start"))
		env_set("preboot", "");
	/* EFI ConIn uses the active stdin; keep serial input available. */
	if (start_keyboard && IS_ENABLED(CONFIG_APPLE_MTP_KEYB) &&
	    IS_ENABLED(CONFIG_CMDLINE))
		run_command("mtpkbd start", 0);

	chosen = ofnode_path("/chosen");
	if (!ofnode_valid(chosen))
		return;

	str = ofnode_read_string(chosen, "aurora,fallback-image");
	if (str && !env_get("aurora_fallback_image"))
		env_set("aurora_fallback_image", str);
	str = ofnode_read_string(chosen, "aurora,fallback-initrd");
	if (str && !env_get("aurora_fallback_initrd"))
		env_set("aurora_fallback_initrd", str);

	apple_loader_initrd(chosen);
}

/*
 * Boot selections the boot loader passes in /chosen, see
 * doc/board/apple/boot.rst. Every variable is only a default: a saved or
 * preboot environment may already have set it.
 */
static const char * const apple_luks_env[][2] = {
	{ "aurora_luks_if", "nvme" },
	/* File names of the existing encrypted /boot installations */
	{ "aurora_luks_kernel", "/boot/Image-j704" },
	{ "aurora_luks_initrd", "/boot/initramfs-j704-crypt.img" },
	{ "aurora_luks_extlinux", "/boot/extlinux/extlinux.conf" },
	{ "aurora_luks_input", "mtpkbd start" },
	/*
	 * Ask up to three times for the passphrase. Boot an extlinux.conf in
	 * the volume if there is one, else the kernel and initramfs named
	 * above; the mapping, and with it the key, is gone before the OS
	 * runs (sysboot tears it down on the way to the OS).
	 */
	{ "aurora_luks_boot",
	  "run aurora_luks_input; "
	  "setenv aurora_luks_dev; "
	  "for try in 1 2 3; do "
	  "if test -z \"${aurora_luks_dev}\" && "
	  "luks unlock ${aurora_luks_if} ${aurora_luks_part}; then "
	  "blkmap get luks-${aurora_luks_if}-${aurora_luks_part} dev "
	  "aurora_luks_dev; fi; done; "
	  "if test -n \"${aurora_luks_dev}\"; then "
	  "setenv bootargs \"${aurora_bootargs} ${aurora_luks_bootargs}\"; "
	  "if test -e blkmap ${aurora_luks_dev} ${aurora_luks_extlinux}; then "
	  "sysboot blkmap ${aurora_luks_dev} any ${scriptaddr} "
	  "${aurora_luks_extlinux}; "
	  "elif load blkmap ${aurora_luks_dev} ${kernel_addr_r} "
	  "${aurora_luks_kernel} && "
	  "load blkmap ${aurora_luks_dev} ${ramdisk_addr_r} "
	  "${aurora_luks_initrd}; then "
	  "setenv aurora_luks_rd ${ramdisk_addr_r}:${filesize}; "
	  "luks close ${aurora_luks_if} ${aurora_luks_part}; "
	  "setenv aurora_luks_dev; "
	  "booti ${kernel_addr_r} ${aurora_luks_rd} ${fdtcontroladdr}; fi; "
	  "if test -n \"${aurora_luks_dev}\"; then "
	  "luks close ${aurora_luks_if} ${aurora_luks_part}; fi; fi; "
	  "echo Encrypted /boot could not be booted" },
};

static const char * const apple_sd_env[][2] = {
	{ "aurora_sd_part", "0:1" },
	/* The boot loader chose this configuration: no other is tried */
	{ "aurora_sd_boot",
	  "setenv bootargs \"${aurora_bootargs}\"; "
	  "sysboot mmc ${aurora_sd_part} any ${scriptaddr} "
	  "${aurora_sd_extlinux}; "
	  "echo SD card configuration ${aurora_sd_extlinux} could not be booted" },
};

static void apple_env_default(const char *name, const char *value)
{
	if (value && !env_get(name))
		env_set(name, value);
}

/*
 * The boot loader may name an encrypted (LUKS2) /boot with
 * aurora,luks-partition, or an extlinux.conf on the SD card with
 * aurora,sd-extlinux-conf. Either replaces the default boot command, the
 * encrypted /boot first. Without them the standard boot scan runs.
 */
static void apple_chosen_boot_late_init(void)
{
	ofnode chosen = ofnode_path("/chosen");
	const char *luks, *sd, *cmd = NULL;
	int i;

	if (!ofnode_valid(chosen))
		return;
	luks = ofnode_read_string(chosen, "aurora,luks-partition");
	sd = ofnode_read_string(chosen, "aurora,sd-extlinux-conf");
	if (!luks && !sd)
		return;

	/* The boot loader's command line, to which the selections add */
	apple_env_default("aurora_bootargs",
			  ofnode_read_string(chosen, "bootargs") ?: "");

	if (luks && IS_ENABLED(CONFIG_CMD_LUKS)) {
		apple_env_default("aurora_luks_part", luks);
		apple_env_default("aurora_luks_bootargs",
				  ofnode_read_string(chosen,
						     "aurora,luks-bootargs") ?: "");
		for (i = 0; i < ARRAY_SIZE(apple_luks_env); i++)
			apple_env_default(apple_luks_env[i][0],
					  apple_luks_env[i][1]);
		cmd = "run aurora_luks_boot";
	} else if (sd && IS_ENABLED(CONFIG_CMD_SYSBOOT)) {
		apple_env_default("aurora_sd_extlinux", sd);
		for (i = 0; i < ARRAY_SIZE(apple_sd_env); i++)
			apple_env_default(apple_sd_env[i][0],
					  apple_sd_env[i][1]);
		cmd = "run aurora_sd_boot";
	}

	if (cmd && !strcmp(env_get("bootcmd") ?: "", CONFIG_BOOTCOMMAND))
		env_set("bootcmd", cmd);
}

/* Prepare the live helper before the keyboard console consumes discovery. */
static int j713_prepare_input(void)
{
	ofnode node, input;
	struct udevice *helper;
	fdt_addr_t data;
	int ret, index;

	node = ofnode_by_compatible(ofnode_null(),
		of_machine_is_compatible("apple,j614s") ?
		"apple,t6041-rtk-helper-asc4" : "apple,t8132-rtk-helper-asc4");
	if (!ofnode_valid(node))
		return -ENODEV;
	ret = uclass_get_device_by_ofnode(UCLASS_MISC, node, &helper);
	if (ret) {
		printf("MTP: failed to initialize input helper (%d)\n", ret);
		return ret;
	}
	input = ofnode_get_parent(ofnode_get_aliases_node("keyboard"));
	if (!ofnode_valid(input))
		return -ENODEV;
	index = ofnode_stringlist_search(input, "reg-names", "data");
	if (index < 0)
		return index;
	data = ofnode_get_addr_index(input, index);
	if (data == FDT_ADDR_T_NONE)
		return -EINVAL;

	return apple_rtkit_helper_prepare_input(helper, (void *)data);
}

/*
 * On a machine with a built-in keyboard, leave the USB-C ports to the OS.
 *
 * U-Boot drives a Type-C port as a USB 2.0 host only: its SuperSpeed lanes
 * stay on the dummy PIPE and the alternate mode set up by the Type-C
 * controller is not followed.  A dock that is already attached and in
 * DisplayPort alternate mode can keep the state of such a session after the
 * OS takes over: the Plugable UD-ULTC4K on a T6000 MacBook Pro then shows
 * only its USB 3 hub, and its USB 2.0 hub, keyboard and mouse stay absent
 * until the cable is reconnected.  The built-in keyboard covers the boot
 * prompt, so "usb start" is not needed to reach it.  Machines without one
 * keep it, and "usb start" still works by hand.
 */
static void apple_typec_late_init(void)
{
	if (!ofnode_valid(ofnode_by_compatible(ofnode_null(),
					       "apple,spi-hid-transport")) &&
	    !ofnode_valid(ofnode_by_compatible(ofnode_null(),
					       "apple,dockchannel-hid")))
		return;

	if (!strcmp(env_get("preboot") ?: "", "usb start"))
		env_set("preboot", "");
}

int board_late_init(void)
{
	bool start_keyboard = true;
	u32 status = 0;
	phys_addr_t addr;

	env_set("storage_interface", blk_get_uclass_name(UCLASS_NVME));
	env_set("fw_dev_part", asahi_esp_devpart());

	/* Ahead of the Aurora EFI defaults, which keep a chosen bootcmd */
	apple_chosen_boot_late_init();

	if (IS_ENABLED(CONFIG_APPLE_MTP_KEYB) &&
	    (of_machine_is_compatible("apple,j713") ||
	     of_machine_is_compatible("apple,j614s"))) {
		int ret = j713_prepare_input();

		start_keyboard = !ret;
		if (ret)
			printf("MTP: input unavailable (%d); continuing boot\n", ret);
	}
	/* T6040 stays with Linux except for the J614s retained handoff. */
	if (of_machine_is_compatible("apple,t6040") &&
	    !of_machine_is_compatible("apple,j614s"))
		start_keyboard = false;
	if (of_machine_is_compatible("apple,t8140") ||
	    of_machine_is_compatible("apple,t8132") ||
	    of_machine_is_compatible("apple,t6040"))
		apple_efi_late_init(start_keyboard);
	else
		apple_typec_late_init();

	/* somewhat based on the Linux Kernel boot requirements:
	 * align by 2M and maximal FDT size 2M
	 */
	status |= !lmb_alloc(SZ_1G, &addr) ? env_set_hex("loadaddr", addr) : 1;
	status |= !lmb_alloc(SZ_2M, &addr) ?
		env_set_hex("fdt_addr_r", addr) : 1;
	status |= !lmb_alloc(SZ_128M, &addr) ?
		env_set_hex("kernel_addr_r", addr) : 1;
	status |= !lmb_alloc(SZ_1G, &addr) ?
		env_set_hex("ramdisk_addr_r", addr) : 1;
	status |= !lmb_alloc(KERNEL_COMP_SIZE, &addr) ?
		env_set_hex("kernel_comp_addr_r", addr) : 1;
	status |= !lmb_alloc(KERNEL_COMP_SIZE, &addr) ?
		env_set_hex("kernel_comp_size", addr) : 1;
	status |= !lmb_alloc(SZ_4M, &addr) ?
		env_set_hex("scriptaddr", addr) : 1;
	status |= !lmb_alloc(SZ_4M, &addr) ?
		env_set_hex("pxefile_addr_r", addr) : 1;

	if (status)
		log_warning("late_init: Failed to set run time variables\n");

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	struct udevice *dev;
	const char *stdoutname;
	int node, ret, imported;

	imported = 0;
	if (IS_ENABLED(CONFIG_APPLE_J713_FDT))
		imported = apple_j713_import_fdt(blob, gd->fdt_blob);
	if (imported < 0) {
		printf("J713: refused OS devicetree runtime import (%d)\n", imported);
		return imported;
	}

	/* Coprocessors handed over running must be described in every case. */
	if (IS_ENABLED(CONFIG_APPLE_MTP_KEYB)) {
		ret = apple_rtkit_helper_fdt_fixup(blob);
		/*
		 * A refused J713 handoff must not reach Linux. Elsewhere a failed
		 * fixup only costs the OS the running session: keep booting.
		 */
		if (ret && (of_machine_is_compatible("apple,j713") ||
			    of_machine_is_compatible("apple,j614s")))
			return ret;
		if (ret)
			printf("RTKit helper OS handoff fixup failed (%d); continuing\n",
			       ret);
	}
	if (IS_ENABLED(CONFIG_APPLE_J713_FDT) && imported) {
		ret = apple_j713_complete_fdt(blob);
		if (ret)
			return ret;
		debug("J713: OS devicetree runtime import complete\n");
	}

	/* A live ANS session retains ownership of firmware and NVMe DMA buffers. */
	ret = apple_nvme_fdt_fixup(blob);
	if (ret) {
		printf("ANS: cannot hand off its DMA buffers to the OS (%d)\n", ret);
		return ret;
	}

	/* The OS finds its firmware on the ESP it was booted from. */
	if (asahi_esp_moved) {
		node = fdt_path_offset(blob, "/chosen");
		if (node >= 0)
			fdt_setprop_string(blob, node, "asahi,efi-system-partition",
					   asahi_esp_uuid);
	}

	/*
	 * Modify the "stdout-path" property under "/chosen" to point
	 * at "/chosen/framebuffer if a keyboard is available and
	 * we're not running under the m1n1 hypervisor.
	 * Developers can override this behaviour by dropping
	 * "vidconsole" from the "stdout" environment variable.
	 */

	/* EL1 means we're running under the m1n1 hypervisor. */
	if (current_el() == 1)
		return 0;

	ret = uclass_find_device(UCLASS_KEYBOARD, 0, &dev);
	if (ret < 0)
		return 0;

	stdoutname = env_get("stdout");
	if (!stdoutname || !strstr(stdoutname, "vidconsole"))
		return 0;

	/* Make sure we actually have a framebuffer. */
	node = fdt_path_offset(blob, "/chosen/framebuffer");
	if (node < 0 || !fdtdec_get_is_enabled(blob, node))
		return 0;

	node = fdt_path_offset(blob, "/chosen");
	if (node < 0)
		return 0;
	fdt_setprop_string(blob, node, "stdout-path", "/chosen/framebuffer");

	return 0;
}
