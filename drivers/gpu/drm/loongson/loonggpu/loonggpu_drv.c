#include <linux/console.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/vga_switcheroo.h>
#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_pciids.h>
#include <drm/drm_crtc_helper.h>

#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_drv.h"
#include "loonggpu_irq.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_helper.h"
#include "loonggpu_lgkcd.h"

#define KMS_DRIVER_MAJOR	1
#define KMS_DRIVER_MINOR	0
#define KMS_DRIVER_PATCHLEVEL	0

int loonggpu_vram_limit;
int loonggpu_vis_vram_limit;
int loonggpu_gart_size = -1; /* auto */
int loonggpu_gtt_size = -1; /* auto */
int loonggpu_moverate = -1; /* auto */
int loonggpu_benchmarking;
int loonggpu_testing;
int loonggpu_disp_priority;
int loonggpu_msi = -1;
int loonggpu_lockup_timeout = 10000;
int loonggpu_gfx_lockup_timeout = 2000;
int loonggpu_dpm = 0;
int loonggpu_runtime_pm = -1;
int loonggpu_vm_size = -1;
int loonggpu_vm_block_size = -1;
int loonggpu_vm_fault_stop;
int loonggpu_vm_debug;
int loonggpu_vram_page_split = 2048;
int loonggpu_vm_update_mode = -1;
int loonggpu_exp_hw_support;
int loonggpu_sched_jobs = 32;
int loonggpu_sched_hw_submission = 2;
int loonggpu_job_hang_limit;
int loonggpu_gpu_recovery = 1; /* auto */
int loonggpu_using_ram; /* using system memory for gpu*/
int loonggpu_noretry = -1;
int loonggpu_virtual_apu = 0;
int loonggpu_support = 1;
int loonggpu_panel_cfg_clk_pol = -1;
int loonggpu_panel_cfg_de_pol = -1;
int loonggpu_gpu_uart = 0;
int loonggpu_ls7a2000_mode_limit = 0;
int loonggpu_ls7a2000_mode_limit_soft = 0;
int loonggpu_hw_cursor = -1;
int loonggpu_write_combine = -1;

#ifdef CONFIG_VDD_LOONGSON_P2P
bool loonggpu_pcie_p2p = true;
#endif

/**
 * DOC: panel_cfg_clk_pol (int)
 * Panel config register clock pol. The default is 1.
 */
MODULE_PARM_DESC(panel_cfg_clk_pol, "Panel config register clock pol");
module_param_named(panel_cfg_clk_pol, loonggpu_panel_cfg_clk_pol, int, 0600);

/**
 * DOC: panel_cfg_de_pol (int)
 * Panel config register de pol. The default is 0.
 */
MODULE_PARM_DESC(panel_cfg_de_pol, "Panel config register de pol");
module_param_named(panel_cfg_de_pol, loonggpu_panel_cfg_de_pol, int, 0600);

MODULE_PARM_DESC(support, "LOONGGPU support (1 = enabled (default), 0 = disabled");
module_param_named(support, loonggpu_support, int, 0444);

MODULE_PARM_DESC(writecombine, "LOONGGPU write combine support (1 = enabled, 0 = disabled)");
module_param_named(writecombine, loonggpu_write_combine, int, 0444);

#ifdef CONFIG_VDD_LOONGSON_P2P
/**
 * DOC: pcie_p2p (bool)
 * Enable PCIe P2P (requires large-BAR). Default value: true (on)
 */
MODULE_PARM_DESC(pcie_p2p, "Enable PCIe P2P (requires large-BAR). (N = off, Y = on(default))");
module_param(loonggpu_pcie_p2p, bool, 0444);
#endif

/**
 * DOC: vramlimit (int)
 * Restrict the total amount of VRAM in MiB for testing.  The default is 0 (Use full VRAM).
 */
MODULE_PARM_DESC(vramlimit, "Restrict VRAM for testing, in megabytes");
module_param_named(vramlimit, loonggpu_vram_limit, int, 0600);

/**
 * DOC: vis_vramlimit (int)
 * Restrict the amount of CPU visible VRAM in MiB for testing.  The default is 0 (Use full CPU visible VRAM).
 */
MODULE_PARM_DESC(vis_vramlimit, "Restrict visible VRAM for testing, in megabytes");
module_param_named(vis_vramlimit, loonggpu_vis_vram_limit, int, 0444);

/**
 * DOC: gartsize (uint)
 * Restrict the size of GART in Mib (32, 64, etc.) for testing. The default is -1 (The size depends on asic).
 */
MODULE_PARM_DESC(gartsize, "Size of GART to setup in megabytes (32, 64, etc., -1=auto)");
module_param_named(gartsize, loonggpu_gart_size, uint, 0600);

/**
 * DOC: gttsize (int)
 * Restrict the size of GTT domain in MiB for testing. The default is -1 (It's VRAM size if 3GB < VRAM < 3/4 RAM,
 * otherwise 3/4 RAM size).
 */
MODULE_PARM_DESC(gttsize, "Size of the GTT domain in megabytes (-1 = auto)");
module_param_named(gttsize, loonggpu_gtt_size, int, 0600);

/**
 * DOC: moverate (int)
 * Set maximum buffer migration rate in MB/s. The default is -1 (8 MB/s).
 */
MODULE_PARM_DESC(moverate, "Maximum buffer migration rate in MB/s. (32, 64, etc., -1=auto, 0=1=disabled)");
module_param_named(moverate, loonggpu_moverate, int, 0600);

/**
 * DOC: benchmark (int)
 * Run benchmarks. The default is 0 (Skip benchmarks).
 */
MODULE_PARM_DESC(benchmark, "Run benchmark");
module_param_named(benchmark, loonggpu_benchmarking, int, 0444);

/**
 * DOC: test (int)
 * Test BO GTT->VRAM and VRAM->GTT GPU copies. The default is 0 (Skip test, only set 1 to run test).
 */
MODULE_PARM_DESC(test, "Run tests");
module_param_named(test, loonggpu_testing, int, 0444);

/**
 * DOC: disp_priority (int)
 * Set display Priority (1 = normal, 2 = high). Only affects non-DC display handling. The default is 0 (auto).
 */
MODULE_PARM_DESC(disp_priority, "Display Priority (0 = auto, 1 = normal, 2 = high)");
module_param_named(disp_priority, loonggpu_disp_priority, int, 0444);

/**
 * DOC: msi (int)
 * To disable Message Signaled Interrupts (MSI) functionality (1 = enable, 0 = disable). The default is -1 (auto, enabled).
 */
MODULE_PARM_DESC(msi, "MSI support (1 = enable, 0 = disable, -1 = auto)");
module_param_named(msi, loonggpu_msi, int, 0444);

/**
 * DOC: lockup_timeout (int)
 * Set GPU scheduler timeout value in ms. Value 0 is invalidated, will be adjusted to 10000.
 * Negative values mean 'infinite timeout' (MAX_JIFFY_OFFSET). The default is 10000.
 */
MODULE_PARM_DESC(lockup_timeout, "GPU lockup timeout in ms > 0 (default 10000)");
module_param_named(lockup_timeout, loonggpu_lockup_timeout, int, 0444);

/**
 * DOC: gfx_lockup_timeout (int)
 * Set GPU gfx scheduler timeout value in ms. Value 0 is invalidated, will be adjusted to 2000.
 * Negative values mean 'infinite timeout' (MAX_JIFFY_OFFSET). The default is 2000.
 */
MODULE_PARM_DESC(gfx_lockup_timeout, "GPU gfx lockup timeout in ms > 0 (default 2000)");
module_param_named(gfx_lockup_timeout, loonggpu_gfx_lockup_timeout, int, 0444);

/**
 * DOC: dpm (int)
 * Override for dynamic power management setting (1 = enable, 0 = disable). The default is -1 (auto).
 */
MODULE_PARM_DESC(dpm, "DPM support (1 = enable, 0 = disable, -1 = auto)");
module_param_named(dpm, loonggpu_dpm, int, 0644);

/**
 * DOC: runpm (int)
 * Override for runtime power management control for dGPUs in PX/HG laptops. The loonggpu driver can dynamically power down
 * the dGPU on PX/HG laptops when it is idle. The default is -1 (auto enable). Setting the value to 0 disables this functionality.
 */
MODULE_PARM_DESC(runpm, "PX runtime pm (1 = force enable, 0 = disable, -1 = PX only default)");
module_param_named(runpm, loonggpu_runtime_pm, int, 0444);

/**
 * DOC: vm_size (int)
 * Override the size of the GPU's per client virtual address space in GiB.  The default is -1 (automatic for each asic).
 */
MODULE_PARM_DESC(vm_size, "VM address space size in gigabytes (default 64GB)");
module_param_named(vm_size, loonggpu_vm_size, int, 0444);

/**
 * DOC: vm_block_size (int)
 * Override VM page table size in bits (default depending on vm_size and hw setup). The default is -1 (automatic for each asic).
 */
MODULE_PARM_DESC(vm_block_size, "VM page table size in bits (default depending on vm_size)");
module_param_named(vm_block_size, loonggpu_vm_block_size, int, 0444);

/**
 * DOC: vm_fault_stop (int)
 * Stop on VM fault for debugging (0 = never, 1 = print first, 2 = always). The default is 0 (No stop).
 */
MODULE_PARM_DESC(vm_fault_stop, "Stop on VM fault (0 = never (default), 1 = print first, 2 = always)");
module_param_named(vm_fault_stop, loonggpu_vm_fault_stop, int, 0444);

/**
 * DOC: vm_debug (int)
 * Debug VM handling (0 = disabled, 1 = enabled). The default is 0 (Disabled).
 */
MODULE_PARM_DESC(vm_debug, "Debug VM handling (0 = disabled (default), 1 = enabled)");
module_param_named(vm_debug, loonggpu_vm_debug, int, 0644);

/**
 * DOC: vm_update_mode (int)
 * Override VM update mode. VM updated by using CPU (0 = never, 1 = Graphics only, 2 = Compute only, 3 = Both). The default
 * is -1 (Only in large BAR(LB) systems Compute VM tables will be updated by CPU, otherwise 0, never).
 */
MODULE_PARM_DESC(vm_update_mode, "VM update using CPU (0 = never (default except for large BAR(LB)), 1 = Graphics only, 2 = Compute only (default for LB), 3 = Both");
module_param_named(vm_update_mode, loonggpu_vm_update_mode, int, 0444);

/**
 * DOC: vram_page_split (int)
 * Override the number of pages after we split VRAM allocations (default 1024, -1 = disable). The default is 1024.
 */
MODULE_PARM_DESC(vram_page_split, "Number of pages after we split VRAM allocations (default 1024, -1 = disable)");
module_param_named(vram_page_split, loonggpu_vram_page_split, int, 0444);

/**
 * DOC: exp_hw_support (int)
 * Enable experimental hw support (1 = enable). The default is 0 (disabled).
 */
MODULE_PARM_DESC(exp_hw_support, "experimental hw support (1 = enable, 0 = disable (default))");
module_param_named(exp_hw_support, loonggpu_exp_hw_support, int, 0444);

/**
 * DOC: sched_jobs (int)
 * Override the max number of jobs supported in the sw queue. The default is 32.
 */
MODULE_PARM_DESC(sched_jobs, "the max number of jobs supported in the sw queue (default 32)");
module_param_named(sched_jobs, loonggpu_sched_jobs, int, 0444);

/**
 * DOC: sched_hw_submission (int)
 * Override the max number of HW submissions. The default is 2.
 */
MODULE_PARM_DESC(sched_hw_submission, "the max number of HW submissions (default 2)");
module_param_named(sched_hw_submission, loonggpu_sched_hw_submission, int, 0444);

/**
 * DOC: job_hang_limit (int)
 * Set how much time allow a job hang and not drop it. The default is 0.
 */
MODULE_PARM_DESC(job_hang_limit, "how much time allow a job hang and not drop it (default 0)");
module_param_named(job_hang_limit, loonggpu_job_hang_limit, int, 0444);

/**
 * DOC: gpu_recovery (int)
 * Set to enable GPU recovery mechanism (1 = enable, 0 = disable). The default is -1 (auto, disabled except SRIOV).
 */
MODULE_PARM_DESC(gpu_recovery, "Enable GPU recovery mechanism, (1 = enable, 0 = disable, -1 = auto)");
module_param_named(gpu_recovery, loonggpu_gpu_recovery, int, 0444);

MODULE_PARM_DESC(loonggpu_using_ram, "Gpu uses memory instead vram"
		 "0: using vram for gpu 1:use system for gpu");
module_param_named(loonggpu_using_ram, loonggpu_using_ram, uint, 0444);

/**
 * DOC: queue_preemption_timeout_ms (int)
 * queue preemption timeout in ms (1 = Minimum, 9000 = default)
 */
int queue_preemption_timeout_ms = 9000;
module_param(queue_preemption_timeout_ms, int, 0644);
MODULE_PARM_DESC(queue_preemption_timeout_ms, "queue preemption timeout in ms (1 = Minimum, 9000 = default)");

/**
 * DOC: debug_evictions(bool)
 * Enable extra debug messages to help determine the cause of evictions
 */
bool debug_evictions;
module_param(debug_evictions, bool, 0644);
MODULE_PARM_DESC(debug_evictions, "enable eviction debug messages (false = default)");

/**
 * DOC: sched_policy (int)
 * Set scheduling policy. Default is HWS(hardware scheduling) with over-subscription.
 * Setting 1 disables over-subscription. Setting 2 disables HWS and statically
 * assigns queues to HQDs.
 */
int sched_policy = KCD_SCHED_POLICY_HWS;
module_param(sched_policy, int, 0444);
MODULE_PARM_DESC(sched_policy,
	"Scheduling policy (0 = HWS (Default), 1 = HWS without over-subscription, 2 = Non-HWS (Used for debugging only)");

/**
 * DOC: hws_max_conc_proc (int)
 * Maximum number of processes that HWS can schedule concurrently. The maximum is the
 * number of VMIDs assigned to the HWS, which is also the default.
 */
int hws_max_conc_proc = -1;
module_param(hws_max_conc_proc, int, 0444);
MODULE_PARM_DESC(hws_max_conc_proc,
	"Max # processes HWS can execute concurrently when sched_policy=0 (0 = no concurrency, #VMIDs for KCD = Maximum(default))");

/**
 *  * DOC: send_sigterm (int)
 *  * Send sigterm to VDD process on unhandled exceptions. Default is not to send sigterm
 *  * but just print errors on dmesg. Setting 1 enables sending sigterm.
 **/
int send_sigterm;
module_param(send_sigterm, int, 0444);
MODULE_PARM_DESC(send_sigterm,
		"Send sigterm to VDD process on unhandled exception (0 = disable, 1 = enable)");

/**
 * DOC: debug_largebar (int)
 * Set debug_largebar as 1 to enable simulating large-bar capability on non-large bar
 * system. This limits the VRAM size reported to ROCm applications to the visible
 * size, usually 256MB.
 * Default value is 0, diabled.
 */
int debug_largebar;
module_param(debug_largebar, int, 0444);
MODULE_PARM_DESC(debug_largebar,
	"Debug large-bar flag used to simulate large-bar capability on non-large bar machine (0 = disable, 1 = enable)");

/**
 * DOC: halt_if_hws_hang (int)
 * Halt if HWS hang is detected. Default value, 0, disables the halt on hang.
 * Setting 1 enables halt on hang.
 */
int halt_if_hws_hang;
module_param(halt_if_hws_hang, int, 0644);
MODULE_PARM_DESC(halt_if_hws_hang, "Halt if HWS hang is detected (0 = off (default), 1 = on)");

/**
 * DOC: loonggpu_cwsr_enable (int)
 * CWSR(compute wave store and resume) allows the GPU to preempt shader execution in
 * the middle of a compute wave. Default is 1 to enable this feature. Setting 0
 * disables it.
 */
int loonggpu_cwsr_enable = 1;
MODULE_PARM_DESC(cwsr_enable, "CWSR enable (0 = Off, 1 = On (Default))");
module_param_named(cwsr_enable, loonggpu_cwsr_enable, int, 0444);

/**
 * DOC: no_system_mem_limit(bool)
 * Disable system memory limit, to support multiple process shared memory
 */
bool no_system_mem_limit;
module_param(no_system_mem_limit, bool, 0644);
MODULE_PARM_DESC(no_system_mem_limit, "disable system memory limit (false = default)");

/**
 * DOC: loonggpu_virtual_apu (int)
 * If set to 1 (enabled), will use virtual apu system.
 */
MODULE_PARM_DESC(virtual_apu, "virtual apu system(1 = enable, 0 = disable).");
module_param_named(virtual_apu, loonggpu_virtual_apu, uint, 0444);

/**
 * DOC: noretry (int)
 * Disable XNACK retry in the SQ by default on LG200 hardware. On ASICs that
 * do not support per-process XNACK this also disables retry page faults.
 * (0 = retry enabled, 1 = retry disabled, -1 auto (default))
 */
MODULE_PARM_DESC(noretry,
	"Disable retry faults (0 = retry enabled, 1 = retry disabled, -1 auto (default))");
module_param_named(noretry, loonggpu_noretry, int, 0644);

/**
 * DOC: gpu_uart (int)
 * Debug GPU UART (0 = disabled, 1 = enabled). The default is 0 (Disabled).
 */
MODULE_PARM_DESC(gpu_uart, "Debug GPU UART (0 = disabled (default), 1 = enabled)");
module_param_named(gpu_uart, loonggpu_gpu_uart, int, 0644);

/**
 * DOC: ls7a2000_mode_limit (bool)
 * Limit the maximum resolution to 1920x1080 for 7A2000.
 * (0 = disabled, 1 = enabled). The default is 0 (Disabled).
 */
MODULE_PARM_DESC(ls7a2000_mode_limit, "Mode limit (0 = disabled (default), 1 = enabled)");
module_param_named(ls7a2000_mode_limit, loonggpu_ls7a2000_mode_limit, int, 0644);

MODULE_PARM_DESC(ls7a2000_mode_limit_soft, "Mode limit (0 = disabled (default), 1 = enabled)");
module_param_named(ls7a2000_mode_limit_soft, loonggpu_ls7a2000_mode_limit_soft, int, 0644);

/**
 * DOC: hw_cursor (int)
 * Enable the hardware cursor on 7A2000 and 2K3000 DC revision 0x10,
 * where it is broken by default.
 * (0 = disabled (default), 1 = enabled).
 */
MODULE_PARM_DESC(hw_cursor, "Hardware cursor on 7A2000/2K3000 rev 0x10 (0 = disabled (default), 1 = enabled)");
module_param_named(hw_cursor, loonggpu_hw_cursor, int, 0644);

static const struct pci_device_id pciidlist[] = {
	{0x0014, 0x7A25, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_LG100},
	{0x0014, 0x7A35, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_LG200},
	{0, 0, 0}
};

static struct pci_dev *loongson_gpu_pdev_current;
static unsigned long pci_gpu_flags_current = CHIP_NO_GPU;

static struct drm_driver kms_driver_common;
static int loongson_vga_pci_register(struct pci_dev *pdev,
				 const struct pci_device_id *ent);

static int loonggpu_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *ent)
{
	unsigned long flags = ent->driver_data;
	int ret;

	if ((flags & LOONGGPU_EXP_HW_SUPPORT) && !loonggpu_exp_hw_support) {
		DRM_INFO("This hardware requires experimental hardware support.\n"
			 "See modparam exp_hw_support\n");
		return -ENODEV;
	}
	ret = pci_enable_device(pdev);
	if (ret)
		return -EINVAL;

	loongson_gpu_pdev_current = pdev;
	pci_gpu_flags_current = flags;

	return 0;
}

static void
loonggpu_pci_remove(struct pci_dev *pdev)
{
	pci_disable_device(pdev);
}

static void
loonggpu_pci_shutdown(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);
	struct loonggpu_device *adev = dev->dev_private;

	/* if we are running in a VM, make sure the device
	 * torn down properly on reboot/shutdown.
	 * unfortunately we can't detect certain
	 * hypervisors so just do this all the time.
	 */
	loonggpu_device_ip_suspend(adev);
}

static int loonggpu_pmops_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	return loonggpu_device_suspend(drm_dev, true, true);
}

static int loonggpu_pmops_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm_dev = pci_get_drvdata(pdev);

	return loonggpu_device_resume(drm_dev, true, true);
}

static int loonggpu_pmops_freeze(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	return loonggpu_device_suspend(drm_dev, false, true);
}

static int loonggpu_pmops_thaw(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	return loonggpu_device_resume(drm_dev, false, true);
}

static int loonggpu_pmops_poweroff(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	return loonggpu_device_suspend(drm_dev, true, true);
}

static int loonggpu_pmops_restore(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	return loonggpu_device_resume(drm_dev, false, true);
}

static int loonggpu_pmops_runtime_suspend(struct device *dev)
{
	pm_runtime_forbid(dev);
	return -EBUSY;
}

static int loonggpu_pmops_runtime_resume(struct device *dev)
{
	return -EINVAL;
}

static int loonggpu_pmops_runtime_idle(struct device *dev)
{
	pm_runtime_forbid(dev);
	return -EBUSY;
}

long loonggpu_drm_ioctl(struct file *filp,
		      unsigned int cmd, unsigned long arg)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev;
	long ret;
	dev = file_priv->minor->dev;
	ret = pm_runtime_get_sync(dev->dev);
	if (ret < 0)
		return ret;

	ret = drm_ioctl(filp, cmd, arg);

	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);
	return ret;
}

/**
 * loongson_vga_pci_devices  -- pci device id info
 *
 * __u32 vendor, device            Vendor and device ID or PCI_ANY_ID
 * __u32 subvendor, subdevice     Subsystem ID's or PCI_ANY_ID
 * __u32 class, class_mask        (class,subclass,prog-if) triplet
 * kernel_ulong_t driver_data     Data private to the driver
 */
static struct pci_device_id loongson_vga_pci_devices[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_LOONGSON, 0x7a36)},
	{PCI_DEVICE(PCI_VENDOR_ID_LOONGSON, 0x7a46)},
	{PCI_DEVICE(PCI_VENDOR_ID_LOONGSON, 0x7a06)},
	{0x0014, 0x9a10, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_LG210},
	{0, 0, 0, 0, 0, 0, 0},
};

MODULE_DEVICE_TABLE(pci, loongson_vga_pci_devices);

static const guid_t prp_guid =
    GUID_INIT(0xdaffd814, 0x6eba, 0x4d8c,
          0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01);

void *loonggpu_get_vram_info(struct device *dev, unsigned long *size)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER };
	const union acpi_object *desc, *uuid, *dcvram_desc = NULL;
	const union acpi_object *el0, *el1, *el2;
	acpi_status status;
	int i;

	status = acpi_evaluate_object_typed(ACPI_HANDLE(dev), "_DSD", NULL,
			&buf, ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status)) {
		dev_info(dev, "ACPI _DSD object not found\n");
		return NULL;
	}

	/* Look for the dc vram UUID */
	desc = buf.pointer;
	for (i = 0; i < desc->package.count; i++) {
		uuid = &desc->package.elements[i];

		if (uuid->type != ACPI_TYPE_BUFFER ||
				uuid->buffer.length != 16) {
			dev_info(dev, "error uuid\n");
			break;
		}
		if (memcmp(uuid->buffer.pointer, &prp_guid, 16) == 0) {
			dcvram_desc = &desc->package.elements[i + 1];
			break;
		}
	}

	if (!dcvram_desc) {
		dev_info(dev, "ACPI dc vram Descriptors not found\n");
		return NULL;
	}

	el0 = &dcvram_desc->package.elements[0];
	if (el0->type == ACPI_TYPE_STRING && !memcmp(el0->string.pointer, "vram_info", 9)) {
		el1 = &dcvram_desc->package.elements[1];
		el2 = &dcvram_desc->package.elements[2];
		if (el1->integer.value == 0 || el2->integer.value == 0) {
			dev_info(dev, "DC vram reserve addr/size error, please update acpi table!!!\n");
			return NULL;
		}
	}
	if (!el1 || !el2) {
		dev_info(dev, "loonggpu dc _DSD package info error\n");
		return NULL;
	}

	*size = el2->integer.value;
	return (void *)el1->integer.value;
}


resource_size_t g_vram_base = 0;
resource_size_t g_vram_size = 0;

struct dc_reg *loonggpu_get_dc_reg(struct pci_dev *pdev)
{
	if (pdev->device == 0x7a36)
		return &ls7a2000_dc_reg;
	else if (pdev->device == 0x7a46)
		return &ls2k3000_dc_reg;
	else if (pdev->device == 0x7a06)
		return &ls7a1000_dc_reg;
	else if (pdev->device == 0x9a10)
		return &ls9a1000_dc_reg;
	else
		return NULL;
}

/**
 * loongson_vga_pci_register -- add pci device
 *
 * @pdev PCI device
 * @ent pci device id
 */
static int loongson_vga_pci_register(struct pci_dev *pdev,
			const struct pci_device_id *ent)

{
	int ret;
	u32 crtc_cfg;
	u32 i;
	u32 hsync_val, vsync_val, crtc_pan;
	resource_size_t dc_rmmio_base;
	resource_size_t dc_rmmio_size;
	void __iomem *dc_rmmio;
	struct drm_device *dev;
	int retry = 0;
	struct pci_dev *gpu_pdev_7a1000;
	u32 sync_dev;
	struct dc_reg *dc_reg;
	struct pci_dev *loongson_gpu_pdev = loongson_gpu_pdev_current;
	unsigned long pci_gpu_flags = pci_gpu_flags_current;
	struct drm_driver *kms_driver;

	kms_driver = kzalloc(sizeof(*kms_driver), GFP_KERNEL);
	if (!kms_driver)
		return -ENOMEM;

	memcpy(kms_driver, &kms_driver_common, sizeof(*kms_driver));

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_kms_driver;

	if (pdev->device == 0x9a10) {
		loongson_gpu_pdev = pdev;
		pci_gpu_flags = ent->driver_data;
		kms_driver->name = "loonggpu";
	}

	/* LG100 GPU uses a "gsgpu" name for the drm driver */
	if (loongson_gpu_pdev && loongson_gpu_pdev->device == 0x7a25)
		kms_driver->name = "gsgpu";

	if (!loongson_gpu_pdev) {
		kms_driver->name = "loongdc";
	}

	dc_reg = loonggpu_get_dc_reg(pdev);

	if (pdev->device == 0x7a06) {
		gpu_pdev_7a1000 = pci_get_device(0x0014, 0x7A15, NULL);
		g_vram_base = pci_resource_start(gpu_pdev_7a1000, 2);
		g_vram_size = pci_resource_len(gpu_pdev_7a1000, 2);
	}

	if (pdev->device != 0x9a10) {
		dc_rmmio_base = pci_resource_start(pdev, 0);
		dc_rmmio_size = pci_resource_len(pdev, 0);
		dc_rmmio = pci_iomap(pdev, 0, dc_rmmio_size);

		if (pdev->device == 0x7a46) {
			for (i = 0; i < 2; i++) {
				crtc_pan = readl(dc_rmmio +
						 dc_reg->crtc_reg[i].panelcfg);
				hsync_val = readl(dc_rmmio +
						 dc_reg->crtc_reg[i].hsync);
				vsync_val = readl(dc_rmmio +
						  dc_reg->crtc_reg[i].vsync);
				crtc_cfg = readl(dc_rmmio +
						 dc_reg->crtc_reg[i].cfg);
				crtc_cfg &= ~CRTC_CFG_ENABLE;
				crtc_pan &= ~CRTC_PANCFG_DE;
				crtc_pan &= ~CRTC_PANCFG_CLKEN;
				hsync_val &= ~CRTC_HSYNC_POLSE;
				vsync_val &= ~CRTC_VSYNC_POLSE;

				writel(crtc_pan, dc_rmmio +
						dc_reg->crtc_reg[i].panelcfg);
				writel(hsync_val, dc_rmmio +
						dc_reg->crtc_reg[i].hsync);
				writel(vsync_val, dc_rmmio +
						dc_reg->crtc_reg[i].vsync);
				writel(crtc_cfg, dc_rmmio +
						dc_reg->crtc_reg[i].cfg);
			}
		} else {
			for (i = 0; i < 2; i++) {
				crtc_cfg = readl(dc_rmmio +
						 dc_reg->crtc_reg[i].cfg);
				crtc_cfg &= ~CRTC_CFG_ENABLE;
				writel(crtc_cfg, dc_rmmio +
						 dc_reg->crtc_reg[i].cfg);
			}
		}

		for (i = 0; i < 2; i++) {
			sync_dev = readl(dc_rmmio +
					 dc_reg->crtc_reg[i].syncdev);
			sync_dev |= (1 << 31);
			writel(sync_dev, dc_rmmio +
					 dc_reg->crtc_reg[i].syncdev);
		}
	}

	ret = aperture_remove_conflicting_devices(0UL, ~0UL, kms_driver->name);
	if (ret)
		goto err_kms_driver;

	dev = drm_dev_alloc(kms_driver, &pdev->dev);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_kms_driver;
	}

#if defined(LG_DDEV_HAS_PDEV)
	dev->pdev = pdev;
#endif

	pci_set_drvdata(pdev, dev);
	if (pdev->device != 0x9a10 && loongson_gpu_pdev)
		pci_set_drvdata(loongson_gpu_pdev, dev);

	dev->dev_private = (void *)loongson_gpu_pdev;
retry_init:
	ret = drm_dev_register(dev, pci_gpu_flags);
	if (ret == -EAGAIN && ++retry <= 3) {
		DRM_INFO("retry init %d\n", retry);
		/* Don't request EX mode too frequently which is attacking */
		msleep(5000);
		goto retry_init;
	} else if (ret)
		goto err_pci;


	return ret;

err_pci:
	pci_disable_device(pdev);
	drm_dev_put(dev);

err_kms_driver:
	kfree(kms_driver);
	return ret;
}

/**
 * loongson_vga_pci_unregister -- release drm device
 *
 * @pdev PCI device
 */
static void loongson_vga_pci_unregister(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);
	struct loonggpu_device *adev = dev->dev_private;

	drm_dev_unregister(dev);
	kfree(dev->driver);
	drm_dev_put(dev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	if (pdev->device != 0x9a10 && adev->family_type != CHIP_NO_GPU)
		pci_set_drvdata(adev->pdev, NULL);
}

static const struct dev_pm_ops loonggpu_pm_ops = {
	.suspend = loonggpu_pmops_suspend,
	.resume = loonggpu_pmops_resume,
	.freeze = loonggpu_pmops_freeze,
	.thaw = loonggpu_pmops_thaw,
	.poweroff = loonggpu_pmops_poweroff,
	.restore = loonggpu_pmops_restore,
	.runtime_suspend = loonggpu_pmops_runtime_suspend,
	.runtime_resume = loonggpu_pmops_runtime_resume,
	.runtime_idle = loonggpu_pmops_runtime_idle,
};

static int loonggpu_flush(struct file *f, fl_owner_t id)
{
	struct drm_file *file_priv = f->private_data;
	struct loonggpu_fpriv *fpriv = file_priv->driver_priv;

	loonggpu_ctx_mgr_entity_flush(&fpriv->ctx_mgr);

	return 0;
}

static const struct file_operations loonggpu_driver_kms_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.flush = loonggpu_flush,
	.release = drm_release,
	.mmap = drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.unlocked_ioctl = loonggpu_drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = loonggpu_kms_compat_ioctl,
#endif
};

static struct drm_driver kms_driver_common = {
	.driver_features = DRIVER_HAVE_IRQ | DRIVER_GEM
		| DRIVER_MODESET | DRIVER_SYNCOBJ
		| DRIVER_SYNCOBJ_TIMELINE
		| DRIVER_RENDER | DRIVER_ATOMIC,
	.load = loonggpu_driver_load_kms,
	.open = loonggpu_driver_open_kms,
	.postclose = loonggpu_driver_postclose_kms,
	.unload = loonggpu_driver_unload_kms,
	.ioctls = loonggpu_ioctls_kms,
	.dumb_create = loonggpu_mode_dumb_create,
	.dumb_map_offset = loonggpu_mode_dumb_mmap,
	.fbdev_probe = loonggpufb_create,
	.fops = &loonggpu_driver_kms_fops,

	.gem_prime_import = loonggpu_gem_prime_import,

	LG_DRM_DRIVER_GEM_PRIME_RES_OBJ_CALLBACK

	.gem_prime_import_sg_table = loonggpu_gem_prime_import_sg_table,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = KMS_DRIVER_MAJOR,
	.minor = KMS_DRIVER_MINOR,
	.patchlevel = KMS_DRIVER_PATCHLEVEL,
};

static struct pci_driver loonggpu_kms_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = pciidlist,
	.probe = loonggpu_pci_probe,
	.remove = loonggpu_pci_remove,
};

/**
 * loongson_vga_pci_driver -- pci driver structure
 *
 * .id_table : must be non-NULL for probe to be called
 * .probe: New device inserted
 * .remove: Device removed
 * .resume: Device suspended
 * .suspend: Device woken up
 */
static struct pci_driver loongson_vga_pci_driver = {
	.name = "loonggpu-dc",
	.id_table = loongson_vga_pci_devices,
	.probe = loongson_vga_pci_register,
	.remove = loongson_vga_pci_unregister,
	.shutdown = loonggpu_pci_shutdown,
	.driver.pm = &loonggpu_pm_ops,
};


/**
 * loonggpu_vga_switchto - switch to the requested GPU
 *
 * @id: GPU to switch to
 *
 * Switch between the discrete GPU and
 * integrated GPU (all asics).
 * Returns 0 on success, error on failure.
 */
static int loonggpu_vga_switchto(enum vga_switcheroo_client_id id)
{
	/* TODO */
	return 0;
}

/**
 * loonggpu_get_vga_client_id - get the client id
 *
 * @pdev: pci device
 *
 * look up whether we are the integrated or discrete GPU (all asics).
 * Returns the client id.
 */
static enum vga_switcheroo_client_id loonggpu_get_vga_client_id(
					struct pci_dev *pdev)
{
	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	struct loonggpu_device *adev = drm_to_adev(drm_dev);

	if (adev->chip == dev_9a1000)
		return VGA_SWITCHEROO_DIS;
	else
		return VGA_SWITCHEROO_IGD;
}

static const struct vga_switcheroo_handler loonggpu_switcheroo_handler = {
	.switchto = loonggpu_vga_switchto,
	.get_client_id = loonggpu_get_vga_client_id,
};

static int __init loonggpu_init(void)
{
	int r;

	if (drm_firmware_drivers_only()) {
		DRM_ERROR("VGACON disables loonggpu kernel modesetting.\n");
		return -EINVAL;
	}

	if (!loonggpu_support)
		return -EINVAL;

	if (!check_vbios_info()) {
		DRM_INFO("loonggpu can not support this board!!!\n");
		return -EINVAL;
	}

	r = loonggpu_sync_init();
	if (r)
		goto error_sync;

	r = loonggpu_fence_slab_init();
	if (r)
		goto error_fence;

	/* Ignore KCD init failures. Normal when CONFIG_VDD_LOONGSON is not set. */
	loonggpu_lgkcd_init();

	DRM_INFO("loonggpu kernel modesetting enabled.\n");

	kms_driver_common.num_ioctls = loonggpu_max_kms_ioctl;
	vga_switcheroo_register_handler(&loonggpu_switcheroo_handler, 0);
	r = pci_register_driver(&loonggpu_kms_pci_driver);
	if (r) {
		goto error_sync;
	}

	r = pci_register_driver(&loongson_vga_pci_driver);
	if (r) {
		goto error_sync;
	}

	return 0;

error_fence:
	loonggpu_sync_fini();

error_sync:
	return r;
}

static void __exit loonggpu_exit(void)
{
	pci_unregister_driver(&loongson_vga_pci_driver);
	pci_unregister_driver(&loonggpu_kms_pci_driver);
	loonggpu_lgkcd_fini();
	loonggpu_sync_fini();
	loonggpu_fence_slab_fini();
}

module_init(loonggpu_init);
module_exit(loonggpu_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
