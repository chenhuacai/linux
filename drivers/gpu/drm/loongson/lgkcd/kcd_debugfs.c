// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2016-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/debugfs.h>
#include <linux/uaccess.h>

#include "kcd_priv.h"

static struct dentry *debugfs_root;

static int kcd_debugfs_open(struct inode *inode, struct file *file)
{
	int (*show)(struct seq_file *, void *) = inode->i_private;

	return single_open(file, show, NULL);
}
static int kcd_debugfs_hang_hws_read(struct seq_file *m, void *data)
{
	seq_puts(m, "echo gpu_id > hang_hws\n");
	return 0;
}

static ssize_t kcd_debugfs_hang_hws_write(struct file *file,
	const char __user *user_buf, size_t size, loff_t *ppos)
{
	struct kcd_node *dev;
	char tmp[16];
	uint32_t gpu_id;
	int ret = -EINVAL;

	memset(tmp, 0, 16);
	if (size >= 16) {
		pr_err("Invalid input for gpu id.\n");
		goto out;
	}
	if (copy_from_user(tmp, user_buf, size)) {
		ret = -EFAULT;
		goto out;
	}
	if (kstrtoint(tmp, 10, &gpu_id)) {
		pr_err("Invalid input for gpu id.\n");
		goto out;
	}
	dev = kcd_device_by_id(gpu_id);
	if (dev) {
		kcd_debugfs_hang_hws(dev);
		ret = size;
	} else
		pr_err("Cannot find device %d.\n", gpu_id);

out:
	return ret;
}

static const struct file_operations kcd_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = kcd_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations kcd_debugfs_hang_hws_fops = {
	.owner = THIS_MODULE,
	.open = kcd_debugfs_open,
	.read = seq_read,
	.write = kcd_debugfs_hang_hws_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void kcd_debugfs_init(void)
{
	debugfs_root = debugfs_create_dir("kcd", NULL);

	debugfs_create_file("mqds", S_IFREG | 0444, debugfs_root,
			    kcd_debugfs_mqds_by_process, &kcd_debugfs_fops);
	debugfs_create_file("hang_hws", S_IFREG | 0200, debugfs_root,
			    kcd_debugfs_hang_hws_read, &kcd_debugfs_hang_hws_fops);
	debugfs_create_file("mem_limit", S_IFREG | 0200, debugfs_root,
			    kcd_debugfs_kcd_mem_limits, &kcd_debugfs_fops);
}

void kcd_debugfs_fini(void)
{
	debugfs_remove_recursive(debugfs_root);
}
