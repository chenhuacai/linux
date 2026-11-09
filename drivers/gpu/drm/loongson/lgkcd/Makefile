# SPDX-License-Identifier: GPL-2.0

LOONGGPU_KCD_OBJECTS := \
	$(KCD_PATH)/kcd_module.o \
	$(KCD_PATH)/kcd_device.o \
	$(KCD_PATH)/kcd_chardev.o \
	$(KCD_PATH)/kcd_doorbell.o \
	$(KCD_PATH)/kcd_topology.o \
	$(KCD_PATH)/kcd_pasid.o \
	$(KCD_PATH)/kcd_flat_memory.o \
	$(KCD_PATH)/kcd_process.o \
	$(KCD_PATH)/kcd_queue.o \
	$(KCD_PATH)/kcd_mqd_manager.o \
	$(KCD_PATH)/kcd_mqd_manager_lg2xx.o \
	$(KCD_PATH)/kcd_kernel_queue.o \
	$(KCD_PATH)/kcd_packet_manager.o \
	$(KCD_PATH)/kcd_packet_manager_lg2xx.o \
	$(KCD_PATH)/kcd_process_queue_manager.o \
	$(KCD_PATH)/kcd_device_queue_manager.o \
	$(KCD_PATH)/kcd_device_queue_manager_lg2xx.o \
	$(KCD_PATH)/kcd_interrupt.o \
	$(KCD_PATH)/kcd_events.o \
	$(KCD_PATH)/kcd_smi_events.o \
	$(KCD_PATH)/kcd_crat.o \
	$(KCD_PATH)/kcd_debug.o \
	$(KCD_PATH)/kcd_ipc.o \
	$(KCD_PATH)/lg2xx_event_interrupt.o

ifneq ($(CONFIG_DEBUG_FS),)
LOONGGPU_KCD_OBJECTS += $(KCD_PATH)/kcd_debugfs.o
endif

ifneq ($(CONFIG_VDD_LOONGSON_SVM),)
LOONGGPU_KCD_OBJECTS += $(KCD_PATH)/kcd_svm.o $(KCD_PATH)/kcd_migrate.o
endif
