/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Present an orange verified-boot state to first-stage PID 1 through
 * /proc/bootconfig, then close the view once PID 1 execs
 * /system/bin/init.
 *
 * Rationale: first-stage AVB handling rejects a verification-disabled
 * vbmeta when the bootloader reports a locked (green) state.  Feeding
 * "orange" to that single reader makes the disabled-verification vbmeta
 * acceptable, while every later reader still sees the real bootloader
 * state.
 *
 * Optionally (CONFIG_ANDROID_DSU_AVB_VBMETA_VIEW) the vbmeta read view is
 * also forced to VERIFICATION_DISABLED, so a stock vbmeta partition can be
 * kept on disk.  SELinux is never touched.
 */

#include <linux/android_dsu_avb.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/string.h>

#include <trace/events/sched.h>

#define SYSTEM_INIT_PATH "/system/bin/init"

static atomic_t first_stage_active = ATOMIC_INIT(1);

bool android_dsu_avb_first_stage(void)
{
	return atomic_read(&first_stage_active) != 0;
}

bool android_dsu_avb_bootconfig_active(void)
{
	return current->pid == 1 && android_dsu_avb_first_stage();
}

static void dsu_avb_process_exec(void *unused, struct task_struct *task,
				 pid_t old_pid, struct linux_binprm *bprm)
{
	(void)unused;
	(void)old_pid;

	if (task->pid != 1 || !bprm || !bprm->filename ||
	    strcmp(bprm->filename, SYSTEM_INIT_PATH))
		return;

	if (atomic_cmpxchg(&first_stage_active, 1, 0) == 1) {
		android_dsu_avb_view_stop();
		pr_info("android-dsu-avb: first-stage orange view closed\n");
	}
}

static int __init android_dsu_avb_init(void)
{
	int error;

	error = register_trace_sched_process_exec(dsu_avb_process_exec, NULL);
	if (error) {
		atomic_set(&first_stage_active, 0);
		pr_err("android-dsu-avb: failed to register init exec gate: %d\n",
		       error);
		return error;
	}

	error = android_dsu_avb_view_start();
	if (error)
		pr_warn("android-dsu-avb: vbmeta read view unavailable (%d); bootconfig view still active\n",
			error);

	pr_info("android-dsu-avb: first-stage orange view enabled\n");
	return 0;
}

subsys_initcall(android_dsu_avb_init);
