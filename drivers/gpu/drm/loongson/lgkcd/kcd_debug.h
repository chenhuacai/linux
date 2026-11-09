/*
 * Copyright 2023 Advanced Micro Devices, Inc.
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

#ifndef KCD_DEBUG_EVENTS_H_INCLUDED
#define KCD_DEBUG_EVENTS_H_INCLUDED

#include "kcd_priv.h"

void kcd_dbg_trap_deactivate(struct kcd_process *target, bool unwind, int unwind_count);
int kcd_dbg_trap_activate(struct kcd_process *target);
int kcd_dbg_ev_query_debug_event(struct kcd_process *process,
			unsigned int *queue_id,
			unsigned int *gpu_id,
			uint64_t exception_clear_mask,
			uint64_t *event_status);
bool kcd_set_dbg_ev_from_interrupt(struct kcd_node *dev,
				   unsigned int pasid,
				   unsigned int queue_id,
				   uint64_t trap_mask,
				   void *exception_data,
				   size_t exception_data_size);
bool kcd_dbg_ev_raise(uint64_t event_mask,
			struct kcd_process *process, struct kcd_node *dev,
			unsigned int source_id, bool use_worker,
			void *exception_data,
			size_t exception_data_size);
int kcd_dbg_trap_disable(struct kcd_process *target);
int kcd_dbg_trap_enable(struct kcd_process *target, uint32_t fd,
			void __user *runtime_info,
			uint32_t *runtime_info_size);
int kcd_dbg_trap_set_flags(struct kcd_process *target, uint32_t *flags);
int kcd_dbg_trap_query_exception_info(struct kcd_process *target,
		uint32_t source_id,
		uint32_t exception_code,
		bool clear_exception,
		void __user *info,
		uint32_t *info_size);
int kcd_dbg_send_exception_to_runtime(struct kcd_process *p,
					unsigned int dev_id,
					unsigned int queue_id,
					uint64_t error_reason);

void debug_event_write_work_handler(struct work_struct *work);
int kcd_dbg_trap_device_snapshot(struct kcd_process *target,
		uint64_t exception_clear_mask,
		void __user *user_info,
		uint32_t *number_of_device_infos,
		uint32_t *entry_size);

void kcd_dbg_set_enabled_debug_exception_mask(struct kcd_process *target,
					uint64_t exception_set_mask);
#endif
