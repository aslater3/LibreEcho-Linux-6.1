// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amazon virtual sensor thermistor
 *
 * Modern IIO consumer for the MT8163 Radar-Puffin/Giza board thermistors.
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define GIZA_PULL_UP_OHM		39000
#define GIZA_CRITICAL_LOW_OHM		195652
#define GIZA_PULL_UP_MV			1800
#define AMAZON_THERMISTOR_DMF		1000

struct amazon_thermistor_point {
	int temp;
	int resistance;
};

/* Giza table 7: Murata NCP15XH103F03RC. */
static const struct amazon_thermistor_point giza_table[] = {
	{ -40, 188500 }, { -35, 144300 }, { -30, 111300 },
	{ -25,  86560 }, { -20,  67790 }, { -15,  53460 },
	{ -10,  42450 }, {  -5,  33930 }, {   0,  27280 },
	{   5,  22070 }, {  10,  17960 }, {  15,  14700 },
	{  20,  12090 }, {  25,  10000 }, {  30,   8312 },
	{  35,   6942 }, {  40,   5826 }, {  45,   4911 },
	{  50,   4158 }, {  55,   3536 }, {  60,   3019 },
	{  65,   2588 }, {  70,   2227 }, {  75,   1924 },
	{  80,   1668 }, {  85,   1451 }, {  90,   1267 },
	{  95,   1110 }, { 100,    975 }, { 105,    860 },
	{ 110,    760 }, { 115,    674 }, { 120,    599 },
	{ 125,    534 },
};

struct amazon_thermistor {
	struct device *dev;
	struct iio_channel *channel;
	struct thermal_zone_device *tzd;
	struct mutex params_lock;
	int offset;
	int alpha;
	int weight;
	int filtered_temp;
	bool filter_valid;
	u32 logical_channel;
};

static int amazon_thermistor_resistance_to_temp(int resistance, int *temp_mc)
{
	int i;
	int temp;

	if (resistance >= giza_table[0].resistance) {
		*temp_mc = giza_table[0].temp * 1000;
		return 0;
	}

	if (resistance <= giza_table[ARRAY_SIZE(giza_table) - 1].resistance) {
		*temp_mc = giza_table[ARRAY_SIZE(giza_table) - 1].temp * 1000;
		return 0;
	}

	for (i = 1; i < ARRAY_SIZE(giza_table); i++) {
		int resistance_hi = giza_table[i - 1].resistance;
		int resistance_lo = giza_table[i].resistance;
		int temp_hi = giza_table[i - 1].temp;
		int temp_lo = giza_table[i].temp;

		if (resistance < resistance_lo)
			continue;

		/* Keep the vendor table's integer-degree interpolation ABI. */
		temp = ((resistance - resistance_lo) * temp_hi +
			(resistance_hi - resistance) * temp_lo) /
		       (resistance_hi - resistance_lo);
		*temp_mc = temp * 1000;
		return 0;
	}

	return -ERANGE;
}

static int amazon_thermistor_read_temp(struct amazon_thermistor *sensor,
				       int *temp)
{
	int critical_mv;
	int resistance;
	int mv;
	int raw_temp;
	int ret;

	ret = iio_read_channel_processed(sensor->channel, &mv);
	if (ret < 0)
		return dev_err_probe(sensor->dev, ret,
				     "failed to read AUXADC channel\n");

	if (mv < 0)
		return -ERANGE;

	critical_mv = GIZA_CRITICAL_LOW_OHM * GIZA_PULL_UP_MV /
		      (GIZA_CRITICAL_LOW_OHM + GIZA_PULL_UP_OHM);

	if (mv > critical_mv) {
		resistance = GIZA_CRITICAL_LOW_OHM;
	} else {
		if (mv >= GIZA_PULL_UP_MV)
			return -ERANGE;

		resistance = GIZA_PULL_UP_OHM * mv /
			     (GIZA_PULL_UP_MV - mv);
	}

	ret = amazon_thermistor_resistance_to_temp(resistance, &raw_temp);
	if (ret < 0)
		return ret;

	/* Preserve the vendor virtual-sensor offset/EMA/weight transform. */
	mutex_lock(&sensor->params_lock);
	raw_temp -= sensor->offset;
	if (!sensor->filter_valid) {
		sensor->filtered_temp = raw_temp;
		sensor->filter_valid = true;
	} else {
		sensor->filtered_temp =
			sensor->alpha * raw_temp +
			(AMAZON_THERMISTOR_DMF - sensor->alpha) *
			sensor->filtered_temp;
		sensor->filtered_temp /= AMAZON_THERMISTOR_DMF;
	}
	*temp = sensor->weight * sensor->filtered_temp /
		AMAZON_THERMISTOR_DMF;
	mutex_unlock(&sensor->params_lock);
	return 0;
}

static int amazon_thermistor_get_temp(struct thermal_zone_device *tzd,
				      int *temp)
{
	struct amazon_thermistor *sensor = tzd->devdata;

	return amazon_thermistor_read_temp(sensor, temp);
}

static struct thermal_zone_device_ops amazon_thermistor_tz_ops = {
	.get_temp = amazon_thermistor_get_temp,
};

static ssize_t temp_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct amazon_thermistor *sensor = dev_get_drvdata(dev);
	int temp;
	int ret;

	ret = amazon_thermistor_read_temp(sensor, &temp);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", temp);
}
static DEVICE_ATTR_RO(temp);

static ssize_t params_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct amazon_thermistor *sensor = dev_get_drvdata(dev);
	int alpha;
	int offset;
	int weight;

	mutex_lock(&sensor->params_lock);
	offset = sensor->offset;
	alpha = sensor->alpha;
	weight = sensor->weight;
	mutex_unlock(&sensor->params_lock);

	return sysfs_emit(buf, "offset=%d alpha=%d weight=%d\n",
			  offset, alpha, weight);
}

static ssize_t params_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct amazon_thermistor *sensor = dev_get_drvdata(dev);
	char param[20];
	int value;

	if (sscanf(buf, "%19s %d", param, &value) != 2)
		return -EINVAL;

	mutex_lock(&sensor->params_lock);
	if (!strcmp(param, "offset"))
		sensor->offset = value;
	else if (!strcmp(param, "alpha"))
		sensor->alpha = value;
	else if (!strcmp(param, "weight"))
		sensor->weight = value;
	else {
		mutex_unlock(&sensor->params_lock);
		return -EINVAL;
	}
	sensor->filter_valid = false;
	mutex_unlock(&sensor->params_lock);

	return count;
}
static DEVICE_ATTR_RW(params);

static struct attribute *amazon_thermistor_attrs[] = {
	&dev_attr_temp.attr,
	&dev_attr_params.attr,
	NULL,
};

static const struct attribute_group amazon_thermistor_group = {
	.attrs = amazon_thermistor_attrs,
};

static void amazon_thermistor_unregister_tz(void *data)
{
	thermal_zone_device_unregister(data);
}

static int amazon_thermistor_read_params(struct device *dev,
					 struct amazon_thermistor *sensor)
{
	const char *sign;
	u32 value;
	int ret;

	ret = of_property_read_u32(dev->of_node, "thermistor,offset", &value);
	if (ret)
		return ret;
	sensor->offset = value;

	ret = of_property_read_string(dev->of_node, "thermistor,offset.sign",
				      &sign);
	if (ret)
		return ret;
	if (!strcmp(sign, "minus"))
		sensor->offset = -sensor->offset;
	else if (strcmp(sign, "plus"))
		return -EINVAL;

	ret = of_property_read_u32(dev->of_node, "thermistor,alpha", &value);
	if (ret)
		return ret;
	sensor->alpha = value;

	ret = of_property_read_u32(dev->of_node, "thermistor,weight", &value);
	if (ret)
		return ret;
	sensor->weight = value;

	ret = of_property_read_u32(dev->of_node, "aux_channel_num",
				   &sensor->logical_channel);
	if (ret)
		return ret;
	if (sensor->logical_channel > 2)
		return -EINVAL;

	return 0;
}

static int amazon_thermistor_probe(struct platform_device *pdev)
{
	struct amazon_thermistor *sensor;
	char *zone_name;
	int ret;

	sensor = devm_kzalloc(&pdev->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &pdev->dev;
	mutex_init(&sensor->params_lock);

	ret = amazon_thermistor_read_params(&pdev->dev, sensor);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid thermistor parameters\n");

	sensor->channel = devm_iio_channel_get(&pdev->dev, NULL);
	if (IS_ERR(sensor->channel))
		return dev_err_probe(&pdev->dev, PTR_ERR(sensor->channel),
				     "failed to get AUXADC channel\n");

	platform_set_drvdata(pdev, sensor);

	ret = devm_device_add_group(&pdev->dev, &amazon_thermistor_group);
	if (ret)
		return ret;

	zone_name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "mtkts_bts%u",
				   sensor->logical_channel);
	if (!zone_name)
		return -ENOMEM;

	sensor->tzd = thermal_zone_device_register(zone_name, 0, 0, sensor,
						   &amazon_thermistor_tz_ops,
						   NULL, 0, 0);
	if (IS_ERR(sensor->tzd))
		return dev_err_probe(&pdev->dev, PTR_ERR(sensor->tzd),
				     "failed to register thermal zone\n");

	ret = devm_add_action_or_reset(&pdev->dev,
				       amazon_thermistor_unregister_tz,
				       sensor->tzd);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id amazon_thermistor_of_match[] = {
	{ .compatible = "amazon,virtual_sensor_thermistor" },
	{ }
};
MODULE_DEVICE_TABLE(of, amazon_thermistor_of_match);

static struct platform_driver amazon_thermistor_driver = {
	.probe = amazon_thermistor_probe,
	.driver = {
		.name = "amazon-virtual-sensor-thermistor",
		.of_match_table = amazon_thermistor_of_match,
	},
};
module_platform_driver(amazon_thermistor_driver);

MODULE_AUTHOR("LibreEcho contributors");
MODULE_DESCRIPTION("Amazon MT8163 virtual sensor thermistor");
MODULE_LICENSE("GPL");
