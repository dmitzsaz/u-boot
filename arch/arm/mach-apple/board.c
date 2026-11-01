// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <dm.h>
#include <dm/uclass-internal.h>
#include <command.h>
#include <efi_loader.h>
#include <env.h>
#include <lmb.h>
#include <nvme.h>
#include <part.h>
#include <usb.h>
#include <linux/ioport.h>
#include <linux/stringify.h>

#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/arch/j704_sd.h>
#include <asm/arch/rtkit.h>

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
		/* PCIE memory window (J704 SD card reader) */
		.virt = 0xb80000000,
		.phys = 0xb80000000,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRE) |
			 PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PCIE ECAM, buses 0..2 */
		.virt = 0x1cb0000000,
		.phys = 0x1cb0000000,
		.size = 0x300000,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN
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

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	ofnode mem = ofnode_null();
	struct resource res;
	int bank = 0, reg;

	/* fdtdec_setup_memory_banksize() stops silently at the array limit. */
	while (ofnode_valid(mem = fdtdec_get_next_memory_node(mem))) {
		for (reg = 0; !ofnode_read_resource(mem, reg, &res); reg++) {
			if (++bank > CONFIG_NR_DRAM_BANKS) {
				log_err("Apple: %d DRAM ranges exceed CONFIG_NR_DRAM_BANKS=%d\n",
					bank, CONFIG_NR_DRAM_BANKS);
				return -E2BIG;
			}
		}
	}

	return fdtdec_setup_memory_banksize();
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
	else if (of_machine_is_compatible("apple,t8140"))
		mem_map = t8140_mem_map;
	else if (of_machine_is_compatible("apple,t8142"))
		mem_map = t8142_mem_map;
	else if (of_machine_is_compatible("apple,t8152") &&
		 of_machine_is_compatible("apple,j873g"))
		mem_map = t8152_mem_map;
	else
		panic("Unsupported SoC\n");

	if (mem_map == t8140_mem_map) {
		/* LMB and EFI see every bank, so the MMU must map every bank. */
		for (bank = 0; bank < CONFIG_NR_DRAM_BANKS; bank++) {
			base = gd->bd->bi_dram[bank].start;
			size = gd->bd->bi_dram[bank].size;
			if (!size)
				break;
			size += base - ALIGN_DOWN(base, SZ_4K);
			base = ALIGN_DOWN(base, SZ_4K);
			mem_map[bank + 1].virt = base;
			mem_map[bank + 1].phys = base;
			mem_map[bank + 1].size = ALIGN(size, SZ_4K);
			mem_map[bank + 1].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
						  PTE_BLOCK_INNER_SHARE;
		}
		fb_slot = bank + 1;
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

static char *asahi_esp_devpart(void)
{
	struct disk_partition info;
	struct blk_desc *nvme_blk;
	const char *uuid = NULL;
	int devnum, len, p, part, ret;
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

	/* ANS is scanned before the SD boot command powers their shared gates. */
	if (IS_ENABLED(CONFIG_J704_SD) && of_machine_is_compatible("apple,j704")) {
		ret = j704_pcie_prepare_power();
		if (ret) {
			log_warning("J704: shared storage power is not ready (%d)\n", ret);
			return devpart;
		}
	}
	nvme_scan_namespace();
	for (devnum = 0, part = 0; ; devnum++) {
		nvme_blk = blk_get_devnum_by_uclass_id(UCLASS_NVME, devnum);
		if (!nvme_blk)
			break;

		dev = dev_get_parent(nvme_blk->bdev);
		if (!device_is_compatible(dev, "apple,nvme-ans2") &&
		    !device_is_compatible(dev, "apple,t8132-nvme-ans2"))
			continue;

		for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
			ret = part_get_info(nvme_blk, p, &info);
			if (ret < 0)
				break;

			if (info.bootable & PART_EFI_SYSTEM_PARTITION) {
				if (uuid && strcasecmp(uuid, info.uuid) == 0) {
					part = p;
					break;
				}
				if (part == 0)
					part = p;
			}
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
 * Aurora Silicon boot flow for the MacBook Neo (A18 Pro, T8140).
 *
 * m1n1 hands U-Boot the Linux devicetree it prepared, with the kernel
 * command line in /chosen/bootargs.  First load the removable-media EFI
 * path from the internal ESP: shim then loads Fedora GRUB and its BLS entries.
 * If that fails, boot the kernel and initramfs named by /chosen fallback
 * properties, then try generic bootflow.  Both direct and EFI boot pass
 * m1n1's devicetree (${fdtcontroladdr}).  With no
 * aurora,fallback-initrd, an initramfs m1n1 already loaded is used.
 *
 * Nothing here is set if the variable already exists, so a saved
 * uboot.env on the ESP can override any of it.
 */
static const char * const t8140_env[][2] = {
	{ "aurora_boot",
	  "echo AURORA_UBOOT_BOOT: EFI on internal ESP; run aurora_efi; "
	  "echo AURORA_UBOOT_BOOT: fallback; run aurora_fallback; "
	  "echo AURORA_UBOOT_BOOT: bootflow; bootflow scan -b; "
	  "echo AURORA_UBOOT_BOOT_FAIL: no bootable entry; run aurora_rescue" },
	{ "aurora_efi",
	  "if test -n \"${fw_dev_part}\"; then "
	  "load nvme ${fw_dev_part} ${kernel_addr_r} EFI/BOOT/BOOTAA64.EFI && "
	  "bootefi ${kernel_addr_r} ${fdtcontroladdr}; fi" },
	/* Keep MTP untouched even when every boot path fails. */
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
static void t8140_loader_initrd(ofnode chosen)
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

static void t8140_late_init(void)
{
	const char *str;
	ofnode chosen;
	int i;

	for (i = 0; i < ARRAY_SIZE(t8140_env); i++) {
		if (!env_get(t8140_env[i][0]))
			env_set(t8140_env[i][0], t8140_env[i][1]);
	}

	/*
	 * Replace the generic Apple defaults unless a saved environment
	 * changed them.  USB is neither started at boot nor a boot target
	 * until the USB-C PHY support has been validated; "usb start" still
	 * works by hand.
	 */
	if (!strcmp(env_get("bootcmd") ?: "", CONFIG_BOOTCOMMAND))
		env_set("bootcmd", "run aurora_boot");
	if (!strcmp(env_get("boot_targets") ?: "", "nvme usb"))
		env_set("boot_targets", "nvme");
	if (!strcmp(env_get("preboot") ?: "", "usb start"))
		env_set("preboot", "");
	/* Keep the added boot time small; a key on the console still stops it. */
	if (!strcmp(env_get("bootdelay") ?: "", __stringify(CONFIG_BOOTDELAY)))
		env_set("bootdelay", "1");

	/* EFI ConIn uses the active stdin; keep serial input available. */
	if (IS_ENABLED(CONFIG_APPLE_MTP_KEYB) && IS_ENABLED(CONFIG_CMDLINE))
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

	t8140_loader_initrd(chosen);
}

static int apple_alloc_kernel(phys_addr_t *addr)
{
	u64 base = gd->bd->bi_dram[0].start;
	u64 size = gd->bd->bi_dram[0].size;

	if (!of_machine_is_compatible("apple,j704"))
		return lmb_alloc(SZ_128M, addr);

	/* Linux 7.3 on J704 reports BAD_PAGE with the generic high load slot.
	 * The measured low slot boots cleanly. Admit it only inside live RAM
	 * and when LMB says the complete, 2-MiB-aligned span is unoccupied.
	 */
	*addr = ALIGN_DOWN(base, SZ_1G) + SZ_256M + SZ_32M;
	if (*addr < base || size < SZ_128M || *addr - base > size - SZ_128M ||
	    lmb_get_free_size(*addr) < SZ_128M)
		return -ENOMEM;
	return lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, SZ_2M, addr, SZ_128M, LMB_NONE);
}

/* Explicit owner DT selection: no SD access or fallback to a different root. */
static bool j704_luks_late_init(void)
{
	const char *part = ofnode_read_string(ofnode_path("/chosen"),
					   "aurora,luks-partition");
	const char *args = ofnode_read_string(ofnode_path("/chosen"),
					   "aurora,luks-bootargs");

	if (!IS_ENABLED(CONFIG_CMD_LUKS) || !part)
		return false;
	if (!env_get("j704_lukspart"))
		env_set("j704_lukspart", part);
	if (!args) {
		env_set("bootcmd", "echo J704: missing encrypted-root bootargs");
		return true;
	}
	env_set("j704_luksargs", args);
	env_set("j704_luksboot",
		"if fdt addr ${fdtcontroladdr} && "
		"fdt get value bootargs /chosen bootargs; then "
		"setenv bootargs ${bootargs} ${j704_luksargs}; "
		"if luks unlock nvme ${j704_lukspart}; then "
		"if load blkmap ${luksdev} ${kernel_addr_r} /boot/Image-j704 && "
		"load blkmap ${luksdev} ${ramdisk_addr_r} /boot/initramfs-j704-crypt.img; then "
		"setenv j704_initrd ${ramdisk_addr_r}:${filesize}; "
		"if luks close; then "
		"booti ${kernel_addr_r} ${j704_initrd} ${fdtcontroladdr}; fi; "
		"else luks close; fi; fi; fi");
	if (!strcmp(env_get("bootcmd") ?: "", CONFIG_BOOTCOMMAND))
		env_set("bootcmd", "run j704_luksboot");
	if (IS_ENABLED(CONFIG_APPLE_MTP_KEYB))
		run_command("mtpkbd start", 0);
	return true;
}

int board_late_init(void)
{
	u32 status = 0;
	phys_addr_t addr;

	env_set("storage_interface", blk_get_uclass_name(UCLASS_NVME));
	env_set("fw_dev_part", asahi_esp_devpart());

	if (of_machine_is_compatible("apple,t8140"))
		t8140_late_init();
	if (of_machine_is_compatible("apple,j704") && !j704_luks_late_init())
		j704_sd_late_init();

	/* somewhat based on the Linux Kernel boot requirements:
	 * align by 2M and maximal FDT size 2M
	 */
	status |= !lmb_alloc(SZ_1G, &addr) ? env_set_hex("loadaddr", addr) : 1;
	status |= !lmb_alloc(SZ_2M, &addr) ?
		env_set_hex("fdt_addr_r", addr) : 1;
	status |= !apple_alloc_kernel(&addr) ?
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

void board_quiesce_devices(void)
{
	if (of_machine_is_compatible("apple,j704"))
		j704_sd_quiesce();

	/* The PHY's OS-prepare callback alone does not halt xHCI DMA. */
	if (IS_ENABLED(CONFIG_USB) && of_machine_is_compatible("apple,j704")) {
		int ret = usb_stop();

		if (ret)
			log_err("J704: USB shutdown failed (%d)\n", ret);
	}
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	struct udevice *dev;
	const char *stdoutname;
	int node, ret;

	if (IS_ENABLED(CONFIG_NVME_APPLE)) {
		ret = apple_nvme_fdt_fixup(blob);
		if (ret)
			return ret;
	}

	/* Coprocessors handed over running must be described in every case. */
	if (IS_ENABLED(CONFIG_APPLE_MTP_KEYB)) {
		ret = apple_rtkit_helper_fdt_fixup(blob);
		if (ret && of_machine_is_compatible("apple,j704"))
			return ret;
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
