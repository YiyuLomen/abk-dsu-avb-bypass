/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ANDROID_DSU_AVB_H
#define _LINUX_ANDROID_DSU_AVB_H

#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_ANDROID_DSU_AVB_BYPASS)
bool android_dsu_avb_bootconfig_active(void);
bool android_dsu_avb_first_stage(void);
#else
static inline bool android_dsu_avb_bootconfig_active(void)
{
	return false;
}

static inline bool android_dsu_avb_first_stage(void)
{
	return false;
}
#endif

#if IS_ENABLED(CONFIG_ANDROID_DSU_AVB_VBMETA_VIEW)
int android_dsu_avb_view_start(void);
void android_dsu_avb_view_stop(void);
#else
static inline int android_dsu_avb_view_start(void)
{
	return 0;
}

static inline void android_dsu_avb_view_stop(void)
{
}
#endif

#endif /* _LINUX_ANDROID_DSU_AVB_H */
