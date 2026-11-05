// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#define APPLE_RTKIT_PWR_STATE_SLEEP	0x01
#define APPLE_RTKIT_PWR_STATE_QUIESCED	0x10
#define APPLE_RTKIT_PWR_STATE_ON	0x20
#define APPLE_RTKIT_PWR_STATE_INIT	0x220

struct apple_rtkit_buffer {
	void *buffer;
	u64 dva;
	size_t size;
	bool is_mapped;
	int endpoint;
};

typedef int (*apple_rtkit_shmem_setup)(void *cookie,
				       struct apple_rtkit_buffer *buf);
typedef void (*apple_rtkit_shmem_destroy)(void *cookie,
					  struct apple_rtkit_buffer *buf);

#include <dm/ofnode.h>

struct apple_rtkit;
struct mbox_chan;

struct apple_rtkit *apple_rtkit_init(struct mbox_chan *chan, void *cookie,
				     apple_rtkit_shmem_setup shmem_setup,
				     apple_rtkit_shmem_destroy shmem_destroy);
void apple_rtkit_free(struct apple_rtkit *rtk);
int apple_rtkit_get_buffers(struct apple_rtkit *rtk,
			   struct apple_rtkit_buffer *buffers, size_t count);
int apple_rtkit_boot(struct apple_rtkit *rtk);
int apple_rtkit_boot_with_sys_ep_mask(struct apple_rtkit *rtk, u32 system_eps);
#define APPLE_RTKIT_BOOT_AP_FIRST BIT(0)
int apple_rtkit_boot_with_options(struct apple_rtkit *rtk, u32 system_eps, u32 flags);
int apple_rtkit_set_ap_power(struct apple_rtkit *rtk, int pwrstate);
int apple_rtkit_poll(struct apple_rtkit *rtk, ulong timeout);
int apple_rtkit_shutdown(struct apple_rtkit *rtk, int pwrstate);

int apple_rtkit_helper_poll(struct udevice *dev, ulong timeout);
/* Only publish a J713 session after its first HID discovery header is pending. */
void apple_rtkit_helper_input_ready(struct udevice *dev);
bool apple_rtkit_helper_input_prepared(struct udevice *dev);
bool apple_rtkit_helper_input_retained(struct udevice *dev);
void apple_rtkit_helper_input_failed(struct udevice *dev);

bool apple_rtkit_helper_allowed(ofnode node);
/* Describe a helper the OS must adopt while it runs (OS device tree fixup). */
int apple_rtkit_helper_fdt_fixup(void *blob);
