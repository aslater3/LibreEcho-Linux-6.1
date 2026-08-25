// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Amazon hardware privacy control
 *
 * This is a descriptor-based port of the vendor amz_priv driver. In
 * particular, the legacy "gpios" property is driven as a raw electrical
 * level: its active-low flag was ignored by of_get_gpio()/gpio_set_value().
 * The Radar-Puffin bright-state line replaces the vendor mute-gpio and uses
 * its declared polarity, matching the old callback's inverse output.
 */

#include <linux/amz_priv.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>

#define DRIVER_NAME "amz_privacy"

struct amz_privacy {
	struct device *dev;
	struct gpio_desc *privacy_gpio;
	struct gpio_desc *bright_state_gpio;
	struct gpio_desc *public_hw_state_gpio;
	struct priv_cb_data *callbacks[PRIV_CB_MAX];
	bool disabled;
	bool hw_latch;
	bool input_handler_registered;
	int privacy_mode_status;
	int shutdown_dialog_status;
	int cur_priv;
	int cur_timer_on;
};

/*
 * The vendor ABI is singleton-based and exports trigger/callback functions.
 * Keep the singleton lock held across an operation so driver unbind cannot
 * invalidate the descriptor pointers underneath an exported call.
 */
static DEFINE_MUTEX(amz_privacy_lock);
static struct amz_privacy *amz_privacy_data;

static void amz_privacy_notify(struct amz_privacy *priv, const char *name)
{
	sysfs_notify(&priv->dev->kobj, NULL, name);
}

static void amz_privacy_set_bright_state(struct amz_privacy *priv, int on)
{
	if (priv->bright_state_gpio)
		gpiod_set_value_cansleep(priv->bright_state_gpio, on);
}

static void amz_privacy_call_callbacks(struct amz_privacy *priv, int on)
{
	int i;

	/* This gating is part of the vendor hardware-latch behavior. */
	if (!priv->hw_latch)
		return;

	for (i = 0; i < PRIV_CB_MAX; i++) {
		struct priv_cb_data *pcd = priv->callbacks[i];

		if (pcd)
			pcd->cb(pcd->cb_priv_data, on);
	}
}

static int amz_privacy_assert_latch(struct amz_privacy *priv)
{
	unsigned int retries = 100;
	int value;

	if (!priv->hw_latch)
		return 0;

	/*
	 * Preserve the vendor latch dance: wait until PUBLIC_HW is private
	 * (raw low), then release the privacy trigger (raw low).
	 */
	while (retries--) {
		value = gpiod_get_raw_value_cansleep(priv->public_hw_state_gpio);
		if (value < 0)
			return value;
		if (!value)
			break;
		usleep_range(10000, 11000);
	}
	if (value)
		return dev_err_probe(priv->dev, -ETIMEDOUT,
				     "privacy latch acknowledgement timed out\n");

	gpiod_set_raw_value_cansleep(priv->privacy_gpio, 0);
	return 0;
}

static int __amz_priv_trigger(struct amz_privacy *priv, int on)
{
	int ret;

	if (priv->disabled)
		return 0;

	on &= 1;

	/* Preserve gpio_set_value() electrical semantics from the old driver. */
	gpiod_set_raw_value_cansleep(priv->privacy_gpio, on);

	/*
	 * Radar-Puffin names the old inverse mute output "bright-state".
	 * Descriptor polarity turns logical privacy-on into its active-low
	 * electrical level.
	 */
	amz_privacy_set_bright_state(priv, on);
	amz_privacy_call_callbacks(priv, on);

	if (on) {
		ret = amz_privacy_assert_latch(priv);
		if (ret)
			return ret;
	}

	/* State changes only after the hardware-latch assertion completes. */
	priv->cur_priv = on;
	amz_privacy_notify(priv, "privacy_state");

	return 0;
}

/*
 * The mute key toggles privacy, and it has to be done here rather than in
 * userspace: privacy_trigger deliberately refuses to let software leave
 * privacy, so that no program can silently un-mute a microphone. A physical
 * key press is the one thing that legitimately may, and only the kernel can
 * tell a real key press from a write by a process.
 *
 * amz_priv_trigger() was already exported for exactly this caller; nothing in
 * this tree provided one, which is why the button lit privacy on and then
 * could never turn it off again.
 */
static void amz_privacy_input_event(struct input_handle *handle,
				    unsigned int type, unsigned int code,
				    int value)
{
	struct amz_privacy *priv;

	if (type != EV_KEY || code != KEY_MUTE || value != 1)
		return;

	mutex_lock(&amz_privacy_lock);
	priv = amz_privacy_data;
	if (priv)
		(void)__amz_priv_trigger(priv, !priv->cur_priv);
	mutex_unlock(&amz_privacy_lock);
}

static int amz_privacy_input_connect(struct input_handler *handler,
				     struct input_dev *dev,
				     const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "amz_privacy";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void amz_privacy_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* Only devices that actually carry KEY_MUTE, so this does not open every
   keyboard on the system. */
static const struct input_device_id amz_privacy_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_MUTE)] = BIT_MASK(KEY_MUTE) },
	},
	{ },
};
MODULE_DEVICE_TABLE(input, amz_privacy_ids);

static struct input_handler amz_privacy_input_handler = {
	.event		= amz_privacy_input_event,
	.connect	= amz_privacy_input_connect,
	.disconnect	= amz_privacy_input_disconnect,
	.name		= "amz_privacy",
	.id_table	= amz_privacy_ids,
};

int amz_priv_trigger(int on)
{
	struct amz_privacy *priv;
	int ret;

	mutex_lock(&amz_privacy_lock);
	priv = amz_privacy_data;
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}

	ret = __amz_priv_trigger(priv, on);
out:
	mutex_unlock(&amz_privacy_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(amz_priv_trigger);

int amz_priv_timer_sysfs(int on)
{
	struct amz_privacy *priv;
	int ret = 0;

	mutex_lock(&amz_privacy_lock);
	priv = amz_privacy_data;
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}

	if (priv->disabled)
		goto out;

	priv->cur_timer_on = on;
	if (on)
		priv->privacy_mode_status = -1;
	amz_privacy_notify(priv, "privacy_timer_on");
out:
	mutex_unlock(&amz_privacy_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(amz_priv_timer_sysfs);

int amz_priv_cb_reg(struct priv_cb_data *pcd)
{
	struct amz_privacy *priv;
	int ret = 0;

	if (!pcd || !pcd->cb || pcd->cb_dest >= PRIV_CB_MAX)
		return -EINVAL;

	mutex_lock(&amz_privacy_lock);
	priv = amz_privacy_data;
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}

	if (!priv->callbacks[pcd->cb_dest])
		priv->callbacks[pcd->cb_dest] = pcd;
out:
	mutex_unlock(&amz_privacy_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(amz_priv_cb_reg);

static ssize_t privacy_trigger_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->privacy_mode_status);
}

static ssize_t privacy_trigger_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);
	int value;

	if (kstrtoint(buf, 10, &value))
		return -EPERM;

	mutex_lock(&amz_privacy_lock);
	priv->privacy_mode_status = value;

	if (value == priv->cur_priv) {
		priv->cur_timer_on = 0;
		amz_privacy_notify(priv, "privacy_timer_on");
		goto out;
	}

	/*
	 * Userspace may enter privacy directly, but privacy_trigger must not
	 * permit software to leave privacy. Value 2 only cancels debounce.
	 */
	if (value == 1 && !priv->cur_timer_on) {
		__amz_priv_trigger(priv, 1);
		priv->cur_timer_on = 0;
		amz_privacy_notify(priv, "privacy_timer_on");
	} else if (value == 2) {
		priv->cur_timer_on = 0;
		amz_privacy_notify(priv, "privacy_timer_on");
	}
out:
	mutex_unlock(&amz_privacy_lock);
	return count;
}

static ssize_t privacy_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->cur_priv);
}

static ssize_t privacy_timer_on_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->cur_timer_on);
}

static ssize_t shutdown_dialog_state_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->shutdown_dialog_status);
}

static ssize_t shutdown_dialog_state_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);
	int value;

	if (kstrtoint(buf, 10, &value))
		return -EPERM;

	mutex_lock(&amz_privacy_lock);
	priv->shutdown_dialog_status = value;
	if (value == 1) {
		__amz_priv_trigger(priv, 0);
		priv->disabled = true;
	} else {
		priv->disabled = false;
	}
	mutex_unlock(&amz_privacy_lock);

	return count;
}

static ssize_t power_button_state_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct amz_privacy *priv = dev_get_drvdata(dev);
	int value;

	if (!priv->bright_state_gpio)
		return -ENODEV;

	/* The legacy file reports the electrical value, not logical polarity. */
	value = gpiod_get_raw_value_cansleep(priv->bright_state_gpio);
	if (value < 0)
		return value;

	return sysfs_emit(buf, "%d\n", value);
}

static DEVICE_ATTR(privacy_trigger, 0664, privacy_trigger_show,
		   privacy_trigger_store);
static DEVICE_ATTR_RO(privacy_state);
static DEVICE_ATTR_RO(privacy_timer_on);
static DEVICE_ATTR(shutdown_dialog_state, 0664,
		   shutdown_dialog_state_show, shutdown_dialog_state_store);
static DEVICE_ATTR(power_button_state, 0664, power_button_state_show, NULL);

static struct attribute *amz_privacy_attrs[] = {
	&dev_attr_privacy_trigger.attr,
	&dev_attr_privacy_state.attr,
	&dev_attr_privacy_timer_on.attr,
	&dev_attr_shutdown_dialog_state.attr,
	&dev_attr_power_button_state.attr,
	NULL,
};

static const struct attribute_group amz_privacy_group = {
	.attrs = amz_privacy_attrs,
};

static struct gpio_desc *
amz_privacy_get_optional_gpio(struct device *dev, const char *property,
			      enum gpiod_flags flags, const char *label)
{
	struct gpio_desc *gpio;

	gpio = devm_gpiod_get_from_of_node(dev, dev->of_node, property, 0,
					   flags, label);
	if (PTR_ERR(gpio) == -ENOENT)
		return NULL;

	return gpio;
}

static int amz_privacy_preserve_output(struct gpio_desc *gpio)
{
	int value;

	value = gpiod_get_raw_value_cansleep(gpio);
	if (value < 0)
		return value;

	return gpiod_direction_output_raw(gpio, value);
}

static int amz_privacy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amz_privacy *priv;
	bool initially_private = false;
	u32 hw_latch = 0;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	device_property_read_u32(dev, "hw_latch", &hw_latch);
	priv->hw_latch = !!hw_latch;

	priv->privacy_gpio = devm_gpiod_get(dev, NULL, GPIOD_ASIS);
	if (IS_ERR(priv->privacy_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->privacy_gpio),
				     "failed to get privacy GPIO\n");

	ret = amz_privacy_preserve_output(priv->privacy_gpio);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to configure privacy GPIO\n");

	priv->bright_state_gpio =
		amz_privacy_get_optional_gpio(dev, "bright-state-gpio",
					      GPIOD_ASIS,
					      "amz-bright-state");
	if (IS_ERR(priv->bright_state_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->bright_state_gpio),
				     "failed to get bright-state GPIO\n");

	/* Also accept the property used by the supplied vendor source. */
	if (!priv->bright_state_gpio)
		priv->bright_state_gpio =
			amz_privacy_get_optional_gpio(dev, "mute-gpio",
						      GPIOD_ASIS,
						      "amz-mute");
	if (IS_ERR(priv->bright_state_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->bright_state_gpio),
				     "failed to get mute GPIO\n");

	if (priv->bright_state_gpio) {
		ret = amz_privacy_preserve_output(priv->bright_state_gpio);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to configure bright-state GPIO\n");
	}

	if (priv->hw_latch) {
		priv->public_hw_state_gpio =
			amz_privacy_get_optional_gpio(dev,
						      "public_hw_st-gpio",
						      GPIOD_IN,
						      "amz-public-hw-state");
		if (IS_ERR(priv->public_hw_state_gpio))
			return dev_err_probe(dev,
					     PTR_ERR(priv->public_hw_state_gpio),
					     "failed to get public-hw GPIO\n");
		if (!priv->public_hw_state_gpio)
			return dev_err_probe(dev, -EINVAL,
					     "hw_latch requires public_hw_st-gpio\n");

		ret = gpiod_get_raw_value_cansleep(priv->public_hw_state_gpio);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to read public-hw GPIO\n");
		initially_private = !ret;
	}

	platform_set_drvdata(pdev, priv);

	mutex_lock(&amz_privacy_lock);
	if (amz_privacy_data) {
		mutex_unlock(&amz_privacy_lock);
		return dev_err_probe(dev, -EBUSY,
				     "only one privacy device is supported\n");
	}
	amz_privacy_data = priv;
	mutex_unlock(&amz_privacy_lock);

	ret = sysfs_create_group(&dev->kobj, &amz_privacy_group);
	if (ret) {
		mutex_lock(&amz_privacy_lock);
		amz_privacy_data = NULL;
		mutex_unlock(&amz_privacy_lock);
		return dev_err_probe(dev, ret,
				     "failed to create privacy attributes\n");
	}

	if (initially_private) {
		mutex_lock(&amz_privacy_lock);
		ret = __amz_priv_trigger(priv, 1);
		mutex_unlock(&amz_privacy_lock);
		if (ret) {
			sysfs_remove_group(&dev->kobj, &amz_privacy_group);
			mutex_lock(&amz_privacy_lock);
			amz_privacy_data = NULL;
			mutex_unlock(&amz_privacy_lock);
			return dev_err_probe(dev, ret,
					     "failed to restore latched privacy\n");
		}
	}

	/*
	 * Registered last: by this point the device can accept a toggle. A
	 * failure here is not fatal -- privacy still works from sysfs and the
	 * boot state is correct, only the key stops toggling it -- so say so
	 * and carry on rather than refusing to probe.
	 */
	ret = input_register_handler(&amz_privacy_input_handler);
	if (ret)
		dev_warn(dev, "mute key handler not registered (%d); the button cannot leave privacy\n",
			 ret);
	else
		priv->input_handler_registered = true;

	dev_info(dev, "registered (hardware latch %s, mute key %s)\n",
		 priv->hw_latch ? "enabled" : "disabled",
		 priv->input_handler_registered ? "bound" : "unbound");
	return 0;
}

static int amz_privacy_remove(struct platform_device *pdev)
{
	struct amz_privacy *priv = platform_get_drvdata(pdev);

	if (priv->input_handler_registered)
		input_unregister_handler(&amz_privacy_input_handler);

	sysfs_remove_group(&pdev->dev.kobj, &amz_privacy_group);

	mutex_lock(&amz_privacy_lock);
	if (amz_privacy_data == priv)
		amz_privacy_data = NULL;
	mutex_unlock(&amz_privacy_lock);

	return 0;
}

static const struct of_device_id amz_privacy_of_match[] = {
	{ .compatible = "amazon,amz-privacy" },
	/* Name-only fallback for unmodified vendor device trees. */
	{ .name = "amz_privacy" },
	{ }
};
MODULE_DEVICE_TABLE(of, amz_privacy_of_match);

static struct platform_driver amz_privacy_driver = {
	.probe = amz_privacy_probe,
	.remove = amz_privacy_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = amz_privacy_of_match,
	},
};
module_platform_driver(amz_privacy_driver);

MODULE_AUTHOR("Amazon Technologies, Inc.; LibreEcho contributors");
MODULE_DESCRIPTION("Amazon Radar-Puffin hardware privacy control");
MODULE_LICENSE("GPL");
