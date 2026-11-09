# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)/../include
ccflags-y += -I$(src)/../loonggpu-bridge

obj-$(CONFIG_DRM_LOONGSON) += loonggpu.o

KCD_PATH := ../lgkcd
include $(src)/../lgkcd/Makefile

BRIDGE_PATH := ../loonggpu-bridge
include $(src)/../loonggpu-bridge/Makefile

loonggpu-y += loonggpu_backlight.o loonggpu_benchmark.o loonggpu_bo_list.o \
	loonggpu_common.o loonggpu_cp.o loonggpu_cs.o loonggpu_ctx.o loonggpu_dc_crtc.o \
	loonggpu_dc_cursor.o loonggpu_dc_drv.o loonggpu_dc_encoder.o loonggpu_dc_hdmi.o \
	loonggpu_dc_dp.o loonggpu_dc_interface.o loonggpu_dc_i2c.o loonggpu_dc_irq.o \
	loonggpu_dc_plane.o loonggpu_dc_vbios.o loonggpu_debugfs.o loonggpu_device.o loonggpu_display.o \
	loonggpu_drv.o loonggpu_fb.o loonggpu_fence.o loonggpu_gart.o loonggpu_gem.o \
	loonggpu_gfx.o loonggpu_gtt_mgr.o loonggpu_hw_sema.o loonggpu_ib.o loonggpu_ids.o \
	loonggpu_ih.o loonggpu_ioc32.o loonggpu_irq.o loonggpu_job.o loonggpu_kms.o \
	loonggpu_ls9a_dp.o loonggpu_ls9a_i2c.o loonggpu_mmu.o loonggpu_mn.o loonggpu_object.o \
	loonggpu_pm.o loonggpu_prime.o loonggpu_queue_mgr.o loonggpu_ring.o loonggpu_sa.o \
	loonggpu_sched.o loonggpu_sync.o loonggpu_test.o loonggpu_trace_points.o \
	loonggpu_ttm.o loonggpu_ttm_buddy.o loonggpu_vm.o loonggpu_vm_it.o \
	loonggpu_vram_mgr.o loonggpu_vram_mgr_buddy.o loonggpu_xdma.o \
	loonggpu_zip.o loonggpu_zip_meta.o loonggpu_helper.o \
	loonggpu_bpipe.o loonggpu_dpipe.o loonggpu_epipe.o loonggpu_pipe.o \
	loonggpu_doorbell_mgr.o loonggpu_dvfs.o loonggpu_dc_video.o loonggpu_dc_irq_msg.o

ifneq ($(CONFIG_VDD_LOONGSON),)
loonggpu-y += $(LOONGGPU_KCD_OBJECTS)
loonggpu-y += loonggpu_lgkcd.o loonggpu_lgkcd_fence.o loonggpu_lgkcd_gfx_lg2xx.o loonggpu_lgkcd_gpuvm.o
endif

loonggpu-y += $(LOONGGPU_BRIDGE_OBJECTS)
