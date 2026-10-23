/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * First-stage vbmeta read-view override.
 *
 * first-stage init (PID 1) reads the top-level vbmeta partition through a
 * block device file.  When the on-disk vbmeta keeps verification enabled
 * (flags == 0), libavb verifies the chained partitions and a locally built
 * boot image fails.  Rather than replacing the vbmeta partition, this
 * helper rewrites only what PID 1 sees: every AVB header read during the
 * first stage gets AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED set, so
 * libavb skips hash and signature checks for that handle.
 *
 * Nothing is written to storage.  The override exists only in the buffer
 * returned to PID 1 and is torn down when PID 1 execs /system/bin/init.
 *
 * The vbmeta header is big-endian; the flags field lives at offset 120, so
 * its least significant byte is at offset 123.
 */

#include <linux/android_dsu_avb.h>
#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/workqueue.h>

#include <asm/ptrace.h>

#define AVB_MAGIC "AVB0"
#define AVB_MAGIC_LEN 4
#define AVB_FLAGS_OFFSET 120
#define AVB_FLAGS_VERIFICATION_DISABLED 2u
#define AVB_HEADER_PROBE_LEN (AVB_FLAGS_OFFSET + 4)
#define VIEW_SLOT_COUNT 8

struct vbmeta_view_slot {
	struct file *file;
	const struct file_operations *orig;
	struct file_operations proxy;
	bool in_use;
};

static struct vbmeta_view_slot view_slots[VIEW_SLOT_COUNT];
static bool kprobe_live;
static atomic64_t view_patched = ATOMIC64_INIT(0);

static ssize_t view_read(struct file *file, char __user *buf, size_t count,
			 loff_t *pos);
static ssize_t view_read_iter(struct kiocb *iocb, struct iov_iter *to);
static int view_release(struct inode *inode, struct file *file);

static inline u32 be32_get(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline void be32_put(u8 *p, u32 value)
{
	p[0] = (u8)(value >> 24);
	p[1] = (u8)(value >> 16);
	p[2] = (u8)(value >> 8);
	p[3] = (u8)value;
}

static struct vbmeta_view_slot *slot_from_ops(
	const struct file_operations *ops)
{
	int index;

	for (index = 0; index < VIEW_SLOT_COUNT; index++) {
		if (READ_ONCE(view_slots[index].in_use) &&
		    &view_slots[index].proxy == ops)
			return &view_slots[index];
	}
	return NULL;
}

/*
 * Force VERIFICATION_DISABLED into an AVB header sitting in user memory.
 * The buffer is left untouched when the magic does not match, so ordinary
 * block device traffic is not affected.
 */
static void patch_avb_header(unsigned long uaddr, size_t length)
{
	u8 header[AVB_HEADER_PROBE_LEN];
	u8 encoded[4];
	u32 flags;

	if (!android_dsu_avb_first_stage())
		return;
	if (length < sizeof(header))
		return;
	if (copy_from_user(header, (void __user *)uaddr, sizeof(header)))
		return;
	if (memcmp(header, AVB_MAGIC, AVB_MAGIC_LEN))
		return;

	flags = be32_get(header + AVB_FLAGS_OFFSET);
	if (flags & AVB_FLAGS_VERIFICATION_DISABLED)
		return;

	flags |= AVB_FLAGS_VERIFICATION_DISABLED;
	be32_put(encoded, flags);
	if (copy_to_user((void __user *)(uaddr + AVB_FLAGS_OFFSET), encoded,
			 sizeof(encoded)))
		return;

	atomic64_inc(&view_patched);
	pr_info("android-dsu-avb: first-stage PID 1 vbmeta read view marked verification-disabled\n");
}

static ssize_t view_read(struct file *file, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct vbmeta_view_slot *slot = slot_from_ops(file->f_op);
	loff_t start = pos ? *pos : -1;
	ssize_t ret;

	if (!slot || !slot->orig->read)
		return -EINVAL;

	ret = slot->orig->read(file, buf, count, pos);
	if (ret > 0 && start == 0)
		patch_avb_header((unsigned long)buf, (size_t)ret);
	return ret;
}

static ssize_t view_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct vbmeta_view_slot *slot = slot_from_ops(iocb->ki_filp->f_op);
	unsigned long uaddr = 0;
	loff_t start = iocb->ki_pos;
	size_t want = iov_iter_count(to);
	ssize_t ret;

	if (!slot || !slot->orig->read_iter)
		return -EINVAL;

	/*
	 * read() on a block device reaches ->read_iter through
	 * new_sync_read(), which uses ITER_UBUF on 6.6; pread() uses
	 * ITER_IOVEC.  iter_iov_addr() covers both user-backed cases, and
	 * 6.6 renamed the iovec member to __iov, so the raw field cannot be
	 * used directly.
	 */
	if (start == 0 && want && to->user_backed)
		uaddr = (unsigned long)iter_iov_addr(to);

	ret = slot->orig->read_iter(iocb, to);

	if (ret > 0 && uaddr && (size_t)ret <= want)
		patch_avb_header(uaddr, (size_t)ret);
	return ret;
}

static int view_release(struct inode *inode, struct file *file)
{
	struct vbmeta_view_slot *slot = slot_from_ops(file->f_op);
	int ret = 0;

	if (!slot)
		return 0;

	if (slot->orig->release)
		ret = slot->orig->release(inode, file);

	WRITE_ONCE(file->f_op, slot->orig);
	WRITE_ONCE(slot->file, NULL);
	WRITE_ONCE(slot->in_use, false);
	return ret;
}

/*
 * Called from the vfs_read kprobe pre-handler, i.e. with preemption
 * disabled: only touches fields under local_irq_save and never sleeps.
 */
static void view_attach(struct file *file)
{
	struct inode *inode = file_inode(file);
	const struct file_operations *ops;
	unsigned long flags;
	int index;

	if (!inode || !S_ISBLK(inode->i_mode))
		return;

	ops = READ_ONCE(file->f_op);
	if (!ops || !ops->read_iter)
		return;

	local_irq_save(flags);
	for (index = 0; index < VIEW_SLOT_COUNT; index++) {
		if (READ_ONCE(view_slots[index].file) == file)
			goto out;
	}
	for (index = 0; index < VIEW_SLOT_COUNT; index++) {
		if (READ_ONCE(view_slots[index].in_use))
			continue;

		view_slots[index].orig = ops;
		view_slots[index].proxy = *ops;
		/*
		 * Only replace ->read when the original file has one.
		 * Installing a non-NULL ->read for a file that used
		 * ->read_iter would divert every read through it; block
		 * devices on 6.6 define read_iter only.
		 */
		if (ops->read)
			view_slots[index].proxy.read = view_read;
		view_slots[index].proxy.read_iter = view_read_iter;
		view_slots[index].proxy.release = view_release;
		WRITE_ONCE(view_slots[index].file, file);
		WRITE_ONCE(view_slots[index].in_use, true);
		WRITE_ONCE(file->f_op, &view_slots[index].proxy);
		goto out;
	}
out:
	local_irq_restore(flags);
}

static int view_vfs_read_pre(struct kprobe *probe, struct pt_regs *regs)
{
	(void)probe;

	if (current->pid != 1)
		return 0;
	if (!android_dsu_avb_first_stage())
		return 0;

	view_attach((struct file *)regs->regs[0]);
	return 0;
}

static struct kprobe view_vfs_read_kprobe = {
	.symbol_name = "vfs_read",
	.pre_handler = view_vfs_read_pre,
};

static void view_teardown(void)
{
	int index;

	unregister_kprobe(&view_vfs_read_kprobe);
	kprobe_live = false;

	for (index = 0; index < VIEW_SLOT_COUNT; index++) {
		struct vbmeta_view_slot *slot = &view_slots[index];

		if (READ_ONCE(slot->file)) {
			WRITE_ONCE(slot->file->f_op, slot->orig);
			WRITE_ONCE(slot->file, NULL);
		}
		WRITE_ONCE(slot->in_use, false);
	}

	pr_info("android-dsu-avb: first-stage AVB read-view window closed (%llu header(s) patched)\n",
		(unsigned long long)atomic64_read(&view_patched));
}

static void view_stop_work_fn(struct work_struct *work)
{
	(void)work;
	if (kprobe_live)
		view_teardown();
}

static DECLARE_WORK(view_stop_work, view_stop_work_fn);

int android_dsu_avb_view_start(void)
{
	int error;

	if (kprobe_live)
		return 0;

	error = register_kprobe(&view_vfs_read_kprobe);
	if (error) {
		pr_err("android-dsu-avb: failed to register vbmeta read view: %d\n",
		       error);
		return error;
	}

	kprobe_live = true;
	pr_info("android-dsu-avb: first-stage vbmeta read view armed\n");
	return 0;
}

/*
 * Called from the sched_process_exec tracepoint, which must not sleep:
 * hand the real teardown (unregister_kprobe uses synchronize_rcu) to a
 * workqueue.
 */
void android_dsu_avb_view_stop(void)
{
	if (!kprobe_live)
		return;
	schedule_work(&view_stop_work);
}
