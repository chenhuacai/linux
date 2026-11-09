// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2014-2022 Advanced Micro Devices, Inc.
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

#include <linux/sched.h>
#include <linux/device.h>
#include "kcd_priv.h"
#include "loonggpu_lgkcd.h"

static int kcd_init(void)
{
	int err;

	/* Verify module parameters */
	if ((sched_policy < KCD_SCHED_POLICY_HWS) ||
		(sched_policy > KCD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION)) {
		pr_err("sched_policy has invalid value\n");
		return -EINVAL;
	}

	/* Verify module parameters */
	if ((max_num_of_queues_per_device < 1) ||
		(max_num_of_queues_per_device >
			KCD_MAX_NUM_OF_QUEUES_PER_DEVICE)) {
		pr_err("max_num_of_queues_per_device must be between 1 to KCD_MAX_NUM_OF_QUEUES_PER_DEVICE\n");
		return -EINVAL;
	}

	err = kcd_chardev_init();
	if (err < 0)
		goto err_ioctl;

	err = kcd_topology_init();
	if (err < 0)
		goto err_topology;

	err = kcd_ipc_init();
	if (err < 0)
		goto err_ipc;

	err = kcd_process_create_wq();
	if (err < 0)
		goto err_create_wq;

	/* Ignore the return value, so that we can continue
	 * to init the KCD, even if procfs isn't craated
	 */
	kcd_procfs_init();

	kcd_debugfs_init();

	return 0;

err_create_wq:
err_ipc:
	kcd_topology_shutdown();
err_topology:
	kcd_chardev_exit();
err_ioctl:
	pr_err("KCD is disabled due to module initialization failure\n");
	return err;
}

static void kcd_exit(void)
{
	kcd_cleanup_processes();
	kcd_debugfs_fini();
	kcd_process_destroy_wq();
	kcd_procfs_shutdown();
	kcd_topology_shutdown();
	kcd_chardev_exit();
}

int kgd2kcd_init(void)
{
	return kcd_init();
}

void kgd2kcd_exit(void)
{
	kcd_exit();
}
