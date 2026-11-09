/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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

#ifndef KCD_SYSFS_H_INCLUDED
#define KCD_SYSFS_H_INCLUDED

/* Capability bits in node properties */
#define VDD_CAP_HOT_PLUGGABLE			0x00000001
#define VDD_CAP_ATS_PRESENT			0x00000002
#define VDD_CAP_SHARED_WITH_GRAPHICS		0x00000004
#define VDD_CAP_QUEUE_SIZE_POW2			0x00000008
#define VDD_CAP_QUEUE_SIZE_32BIT		0x00000010
#define VDD_CAP_QUEUE_IDLE_EVENT		0x00000020
#define VDD_CAP_VA_LIMIT			0x00000040

#define VDD_CAP_ASIC_REVISION_MASK		0x03c00000
#define VDD_CAP_ASIC_REVISION_SHIFT		22
#define VDD_CAP_VDD_PROFILE_FULL	        0x04000000
#define VDD_CAP_SVMAPI_SUPPORTED		0x08000000
#define VDD_CAP_FLAGS_COHERENTHOSTACCESS	0x10000000
#define VDD_CAP_RESERVED			0xe00f8000
#define VDD_CAP_DOORBELL_TYPE_TOTALBITS_MASK	0x00003000
#define VDD_CAP_DOORBELL_TYPE_TOTALBITS_SHIFT	12

#define VDD_CAP_DOORBELL_TYPE_1_0		0x1
#define VDD_CAP_TRAP_DEBUG_SUPPORT              0x00008000

/* debug_prop bits in node properties */
#define VDD_DBG_DISPATCH_INFO_ALWAYS_VALID      0x00000400

/* Heap types in memory properties */
#define VDD_MEM_HEAP_TYPE_SYSTEM	0
#define VDD_MEM_HEAP_TYPE_FB_PUBLIC	1
#define VDD_MEM_HEAP_TYPE_FB_PRIVATE	2
#define VDD_MEM_HEAP_TYPE_GPU_LDS	4
#define VDD_MEM_HEAP_TYPE_GPU_SCRATCH	5

/* Flag bits in memory properties */
#define VDD_MEM_FLAGS_HOT_PLUGGABLE		0x00000001
#define VDD_MEM_FLAGS_NON_VOLATILE		0x00000002
#define VDD_MEM_FLAGS_RESERVED			0xfffffffc

/* Cache types in cache properties */
#define VDD_CACHE_TYPE_DATA		0x00000001
#define VDD_CACHE_TYPE_INSTRUCTION	0x00000002
#define VDD_CACHE_TYPE_CPU		0x00000004
#define VDD_CACHE_TYPE_VDDCU		0x00000008
#define VDD_CACHE_TYPE_RESERVED		0xfffffff0

/* Link types in IO link properties (matches CRAT link types) */
#define VDD_IOLINK_TYPE_UNDEFINED	0
#define VDD_IOLINK_TYPE_PCIEXPRESS	1
#define VDD_IOLINK_TYPE_OTHER		16

/* Flag bits in IO link properties (matches CRAT flags, excluding the
 * bi-directional flag, which is not offially part of the CRAT spec, and
 * only used internally in KCD)
 */
#define VDD_IOLINK_FLAGS_ENABLED		(1 << 0)
#define VDD_IOLINK_FLAGS_NON_COHERENT		(1 << 1)
#define VDD_IOLINK_FLAGS_NO_ATOMICS_32_BIT	(1 << 2)
#define VDD_IOLINK_FLAGS_NO_ATOMICS_64_BIT	(1 << 3)
#define VDD_IOLINK_FLAGS_NO_PEER_TO_PEER_DMA	(1 << 4)
#define VDD_IOLINK_FLAGS_RESERVED		0xffffffe0

#endif
