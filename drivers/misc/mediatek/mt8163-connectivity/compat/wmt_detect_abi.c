// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT8163-only compatibility endpoint for the stock WMT loader.
 *
 * The vendor common-detect driver probes optional removable SDIO combo
 * chips and dynamically initializes separate connectivity modules.  Neither
 * operation applies to this built-in integrated CONSYS port, but v181
 * wmt_loader still requires the fixed /dev/wmtdetect ioctl ABI.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>

#include "mtk_wcn_cmb_stub.h"

#define WMT_DETECT_MAJOR	154
#define WMT_DETECT_NAME		"wmtdetect"
#define WMT_IOC_MAGIC		'w'

#define COMBO_IOCTL_GET_CHIP_ID		_IOR(WMT_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_SET_CHIP_ID		_IOW(WMT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_EXT_CHIP_DETECT	_IOR(WMT_IOC_MAGIC, 2, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID	_IOR(WMT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT	_IOR(WMT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP	_IOR(WMT_IOC_MAGIC, 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON	_IOR(WMT_IOC_MAGIC, 6, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_OFF	_IOR(WMT_IOC_MAGIC, 7, int)
#define COMBO_IOCTL_DO_SDIO_AUTOK	_IOR(WMT_IOC_MAGIC, 8, int)

static dev_t wmt_detect_devt = MKDEV(WMT_DETECT_MAJOR, 0);
static struct cdev wmt_detect_cdev;
static struct class *wmt_detect_class;

static long wmt_detect_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case COMBO_IOCTL_GET_CHIP_ID:
	case COMBO_IOCTL_GET_SOC_CHIP_ID:
		return mtk_wcn_wmt_chipid_query();
	case COMBO_IOCTL_SET_CHIP_ID:
		mtk_wcn_wmt_set_chipid(arg);
		return 0;
	case COMBO_IOCTL_DO_MODULE_INIT:
	case COMBO_IOCTL_MODULE_CLEANUP:
	case COMBO_IOCTL_EXT_CHIP_PWR_OFF:
		/* The integrated drivers are built in and already initialized. */
		return 0;
	case COMBO_IOCTL_EXT_CHIP_DETECT:
	case COMBO_IOCTL_EXT_CHIP_PWR_ON:
	case COMBO_IOCTL_DO_SDIO_AUTOK:
		return -EOPNOTSUPP;
	default:
		/* Match the vendor endpoint's treatment of unknown commands. */
		return 0;
	}
}

static ssize_t wmt_detect_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *offset)
{
	return 0;
}

static ssize_t wmt_detect_write(struct file *file,
				const char __user *buffer, size_t count,
				loff_t *offset)
{
	return 0;
}

static const struct file_operations wmt_detect_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.read = wmt_detect_read,
	.write = wmt_detect_write,
	.unlocked_ioctl = wmt_detect_ioctl,
	.llseek = no_llseek,
};

static int __init wmt_detect_abi_init(void)
{
	struct device *device;
	int ret;

	ret = register_chrdev_region(wmt_detect_devt, 1, "mtk_wcn_detect");
	if (ret)
		return ret;

	cdev_init(&wmt_detect_cdev, &wmt_detect_fops);
	wmt_detect_cdev.owner = THIS_MODULE;
	ret = cdev_add(&wmt_detect_cdev, wmt_detect_devt, 1);
	if (ret)
		goto unregister_region;

	wmt_detect_class = class_create(THIS_MODULE, WMT_DETECT_NAME);
	if (IS_ERR(wmt_detect_class)) {
		ret = PTR_ERR(wmt_detect_class);
		wmt_detect_class = NULL;
		goto delete_cdev;
	}

	device = device_create(wmt_detect_class, NULL, wmt_detect_devt, NULL,
			       WMT_DETECT_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}

	return 0;

destroy_class:
	class_destroy(wmt_detect_class);
	wmt_detect_class = NULL;
delete_cdev:
	cdev_del(&wmt_detect_cdev);
unregister_region:
	unregister_chrdev_region(wmt_detect_devt, 1);
	return ret;
}
module_init(wmt_detect_abi_init);

