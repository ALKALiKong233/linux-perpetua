// SPDX-License-Identifier: GPL-2.0
/*
 * host.c - DesignWare USB3 DRD Controller Host Glue
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - https://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 */

#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/bitfield.h>

#include "../host/xhci-port.h"
#include "../host/xhci-ext-caps.h"
#include "../host/xhci-caps.h"
#include "../host/xhci-plat.h"
#include "core.h"
#include "io.h"

#define XHCI_HCSPARAMS1		0x4
#define XHCI_PORTSC_BASE	0x400

/**
 * dwc3_power_off_all_roothub_ports - Power off all Root hub ports
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_power_off_all_roothub_ports(struct dwc3 *dwc)
{
	void __iomem *xhci_regs;
	u32 op_regs_base;
	int port_num;
	u32 offset;
	u32 reg;
	int i;

	/* xhci regs are not mapped yet, do it temporarily here */
	if (dwc->xhci_resources[0].start) {
		if (dwc->xhci_resources[0].flags & IORESOURCE_MEM_NONPOSTED)
			xhci_regs = ioremap_np(dwc->xhci_resources[0].start, DWC3_XHCI_REGS_END);
		else
			xhci_regs = ioremap(dwc->xhci_resources[0].start, DWC3_XHCI_REGS_END);
		if (!xhci_regs) {
			dev_err(dwc->dev, "Failed to ioremap xhci_regs\n");
			return;
		}

		op_regs_base = FIELD_GET(HC_LENGTH, readl(xhci_regs));
		reg = readl(xhci_regs + XHCI_HCSPARAMS1);
		port_num = FIELD_GET(HCS_MAX_PORTS, reg);

		for (i = 1; i <= port_num; i++) {
			offset = op_regs_base + XHCI_PORTSC_BASE + 0x10 * (i - 1);
			reg = readl(xhci_regs + offset);
			reg &= ~PORT_POWER;
			writel(reg, xhci_regs + offset);
		}

		iounmap(xhci_regs);
	} else {
		dev_err(dwc->dev, "xhci base reg invalid\n");
	}
}

static void dwc3_xhci_plat_start(struct usb_hcd *hcd)
{
	struct platform_device *pdev;
	struct dwc3 *dwc;

	if (!usb_hcd_is_primary_hcd(hcd))
		return;

	pdev = to_platform_device(hcd->self.controller);
	dwc = dev_get_drvdata(pdev->dev.parent);

	dwc3_enable_susphy(dwc, true);
}

static void dwc3_host_recovery_work(struct work_struct *work)
{
	struct dwc3 *dwc = recovery_work_to_dwc(work);
	unsigned long flags;
	u32 reg;
	int ret;
	int i;

	mutex_lock(&dwc->mutex);

	spin_lock_irqsave(&dwc->lock, flags);
	if (dwc->current_dr_role != DWC3_GCTL_PRTCAP_HOST ||
	    dwc->desired_dr_role != DWC3_GCTL_PRTCAP_HOST) {
		spin_unlock_irqrestore(&dwc->lock, flags);
		goto out_unlock;
	}
	spin_unlock_irqrestore(&dwc->lock, flags);

	if (!dwc->host_recovery_last ||
	    time_after(jiffies, dwc->host_recovery_last + 60 * HZ))
		dwc->host_recovery_attempts = 0;

	if (dwc->host_recovery_attempts >= 3) {
		dev_err(dwc->dev,
			"host recovery limit reached; waiting for a new connection\n");
		goto out_unlock;
	}

	dwc->host_recovery_attempts++;
	dwc->host_recovery_last = jiffies;

	ret = pm_runtime_resume_and_get(dwc->dev);
	if (ret < 0) {
		dev_err(dwc->dev, "failed to resume for host recovery: %d\n", ret);
		goto out_unlock;
	}

	dev_warn(dwc->dev, "recovering unresponsive xHCI host controller\n");
	dwc3_host_exit(dwc);

	if (dwc->glue_ops && dwc->glue_ops->recover_host) {
		ret = dwc->glue_ops->recover_host(dwc);
		if (ret) {
			dev_err(dwc->dev,
				"platform host recovery failed: %d\n", ret);
			goto out_pm;
		}
	} else {
		/* Recreate the xHCI child after resetting the shared DWC3 core. */
		reg = dwc3_readl(dwc, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		dwc3_writel(dwc, DWC3_GCTL, reg);
		msleep(100);
		reg = dwc3_readl(dwc, DWC3_GCTL);
		reg &= ~DWC3_GCTL_CORESOFTRESET;
		dwc3_writel(dwc, DWC3_GCTL, reg);
	}

	spin_lock_irqsave(&dwc->lock, flags);
	dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_HOST, false);
	spin_unlock_irqrestore(&dwc->lock, flags);

	for (i = 0; i < dwc->num_usb2_ports; i++)
		phy_set_mode(dwc->usb2_generic_phy[i], PHY_MODE_USB_HOST);
	for (i = 0; i < dwc->num_usb3_ports; i++)
		phy_set_mode(dwc->usb3_generic_phy[i], PHY_MODE_USB_HOST);

	ret = dwc3_host_init(dwc);
	if (ret)
		dev_err(dwc->dev, "failed to recover host controller: %d\n", ret);
	else
		dev_info(dwc->dev, "xHCI host controller recovered\n");

out_pm:
	pm_runtime_mark_last_busy(dwc->dev);
	pm_runtime_put_autosuspend(dwc->dev);

out_unlock:
	mutex_unlock(&dwc->mutex);
}

static void dwc3_xhci_plat_hc_died(struct usb_hcd *hcd)
{
	struct platform_device *pdev = to_platform_device(hcd->self.controller);
	struct dwc3 *dwc = dev_get_drvdata(pdev->dev.parent);

	if (!dwc || !dwc->recover_host_on_hc_died)
		return;

	mod_delayed_work(system_long_wq, &dwc->host_recovery_work,
			 msecs_to_jiffies(250));
}

static const struct xhci_plat_priv dwc3_xhci_plat_quirk = {
	.plat_start = dwc3_xhci_plat_start,
	.plat_hc_died = dwc3_xhci_plat_hc_died,
	.plat_hc_error = dwc3_xhci_plat_hc_died,
};

void dwc3_host_recovery_init(struct dwc3 *dwc)
{
	INIT_DELAYED_WORK(&dwc->host_recovery_work, dwc3_host_recovery_work);
}

void dwc3_host_recovery_cancel(struct dwc3 *dwc)
{
	cancel_delayed_work_sync(&dwc->host_recovery_work);
}

static void dwc3_host_fill_xhci_irq_res(struct dwc3 *dwc,
					int irq, char *name)
{
	struct platform_device *pdev = to_platform_device(dwc->dev);
	struct device_node *np = dev_of_node(&pdev->dev);

	dwc->xhci_resources[1].start = irq;
	dwc->xhci_resources[1].end = irq;
	dwc->xhci_resources[1].flags = IORESOURCE_IRQ | irq_get_trigger_type(irq);
	if (!name && np)
		dwc->xhci_resources[1].name = of_node_full_name(pdev->dev.of_node);
	else
		dwc->xhci_resources[1].name = name;
}

static int dwc3_host_get_irq(struct dwc3 *dwc)
{
	struct platform_device	*dwc3_pdev = to_platform_device(dwc->dev);
	int irq;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "host");
	if (irq > 0) {
		dwc3_host_fill_xhci_irq_res(dwc, irq, "host");
		goto out;
	}

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "dwc_usb3");
	if (irq > 0) {
		dwc3_host_fill_xhci_irq_res(dwc, irq, "dwc_usb3");
		goto out;
	}

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq(dwc3_pdev, 0);
	if (irq > 0)
		dwc3_host_fill_xhci_irq_res(dwc, irq, NULL);

out:
	return irq;
}

int dwc3_host_init(struct dwc3 *dwc)
{
	struct property_entry	props[6];
	struct platform_device	*xhci;
	int			ret, irq;
	int			prop_idx = 0;

	/*
	 * Some platforms need to power off all Root hub ports immediately after DWC3 set to host
	 * mode to avoid VBUS glitch happen when xhci get reset later.
	 */
	dwc3_power_off_all_roothub_ports(dwc);

	irq = dwc3_host_get_irq(dwc);
	if (irq < 0)
		return irq;

	xhci = platform_device_alloc("xhci-hcd", PLATFORM_DEVID_AUTO);
	if (!xhci) {
		dev_err(dwc->dev, "couldn't allocate xHCI device\n");
		return -ENOMEM;
	}

	xhci->dev.parent	= dwc->dev;

	dwc->xhci = xhci;

	ret = platform_device_add_resources(xhci, dwc->xhci_resources,
						DWC3_XHCI_RESOURCES_NUM);
	if (ret) {
		dev_err(dwc->dev, "couldn't add resources to xHCI device\n");
		goto err;
	}

	memset(props, 0, sizeof(struct property_entry) * ARRAY_SIZE(props));

	props[prop_idx++] = PROPERTY_ENTRY_BOOL("xhci-sg-trb-cache-size-quirk");

	props[prop_idx++] = PROPERTY_ENTRY_BOOL("write-64-hi-lo-quirk");

	if (dwc->usb3_lpm_capable)
		props[prop_idx++] = PROPERTY_ENTRY_BOOL("usb3-lpm-capable");

	if (dwc->usb2_lpm_disable)
		props[prop_idx++] = PROPERTY_ENTRY_BOOL("usb2-lpm-disable");

	/**
	 * WORKAROUND: dwc3 revisions <=3.00a have a limitation
	 * where Port Disable command doesn't work.
	 *
	 * The suggested workaround is that we avoid Port Disable
	 * completely.
	 *
	 * This following flag tells XHCI to do just that.
	 */
	if (DWC3_VER_IS_WITHIN(DWC3, ANY, 300A))
		props[prop_idx++] = PROPERTY_ENTRY_BOOL("quirk-broken-port-ped");

	props[prop_idx++] = PROPERTY_ENTRY_U16("num-hc-interrupters",
					       dwc->num_hc_interrupters);

	if (prop_idx) {
		ret = device_create_managed_software_node(&xhci->dev, props, NULL);
		if (ret) {
			dev_err(dwc->dev, "failed to add properties to xHCI\n");
			goto err;
		}
	}

	ret = platform_device_add_data(xhci, &dwc3_xhci_plat_quirk,
				       sizeof(struct xhci_plat_priv));
	if (ret)
		goto err;

	ret = platform_device_add(xhci);
	if (ret) {
		dev_err(dwc->dev, "failed to register xHCI device\n");
		goto err;
	}

	if (dwc->sys_wakeup) {
		/* Restore wakeup setting if switched from device */
		device_wakeup_enable(dwc->sysdev);

		/* Pass on wakeup setting to the new xhci platform device */
		device_init_wakeup(&xhci->dev, true);
	}

	return 0;
err:
	platform_device_put(xhci);
	return ret;
}
EXPORT_SYMBOL_GPL(dwc3_host_init);

void dwc3_host_exit(struct dwc3 *dwc)
{
	if (dwc->sys_wakeup)
		device_init_wakeup(&dwc->xhci->dev, false);

	dwc3_enable_susphy(dwc, true);
	platform_device_unregister(dwc->xhci);
	dwc->xhci = NULL;
}
EXPORT_SYMBOL_GPL(dwc3_host_exit);
