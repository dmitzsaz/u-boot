/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ASM_ARCH_APPLE_J704_SD_H
#define __ASM_ARCH_APPLE_J704_SD_H

#if IS_ENABLED(CONFIG_J704_SD)
void j704_sd_late_init(void);
void j704_sd_quiesce(void);
#else
static inline void j704_sd_late_init(void) {}
static inline void j704_sd_quiesce(void) {}
#endif

#endif
