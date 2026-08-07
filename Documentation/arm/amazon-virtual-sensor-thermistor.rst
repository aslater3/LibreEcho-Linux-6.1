Amazon MT8163 virtual thermistor support
========================================

Status: implemented.

Radar-Puffin/Giza has three thermistors described by
``amazon,virtual_sensor_thermistor`` nodes. Logical sensor indices 0, 1 and 2
are connected to MT8163 AUXADC hardware inputs 0, 14 and 15 respectively.
The AUXADC provider supports the MT8163 compatible, its ``auxadc-main`` clock
name, ``#io-channel-cells`` and the verified 12-bit 0--1500 mV conversion.

``drivers/thermal/amazon_virtual_sensor_thermistor.c`` is the IIO consumer. It
uses the Giza board electrical values from the vendor kernel:

* 39,000 ohm pull-up;
* 195,652 ohm critical-low clamp;
* 1,800 mV pull-up voltage; and
* table 7, Murata NCP15XH103F03RC, from -40 to 125 degrees Celsius.

ADC and probe errors are returned to callers; the driver does not substitute a
constant temperature. Voltage is converted to resistance with the vendor
divider formula and then interpolated through the board table. Readings are
reported in millidegrees Celsius through a Linux thermal zone named
``mtkts_bts0``, ``mtkts_bts1`` or ``mtkts_bts2``.

Vendor userspace ABI
--------------------

Each platform node retains these attributes:

``temp`` (read-only)
  Current calibrated temperature in millidegrees Celsius.

``params`` (read/write)
  Reads as ``offset=N alpha=N weight=N``. A single value is updated with
  ``offset N``, ``alpha N`` or ``weight N``.

The device-tree offset sign and the Radar-Puffin/Giza offset, alpha and weight
values are preserved. These writable values remain virtual-sensor policy
metadata, as in the vendor driver; they do not replace or bias the calibrated
physical thermistor reading exposed by ``temp`` and the thermal zone.

Configuration
-------------

The relevant symbols are ``CONFIG_MEDIATEK_MT6577_AUXADC`` and
``CONFIG_AMAZON_VIRTUAL_SENSOR_THERMISTOR``. The reusable ARM fragment is
``arch/arm/configs/libreecho_mt8163_thermistor.config``.
