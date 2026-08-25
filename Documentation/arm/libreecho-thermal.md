# LibreEcho MT8163 thermal policy

An evaluation of the thermal behaviour this tree inherited, what is wrong with
it, and the policy replacing it. Everything below is read out of the source; the
section at the end lists what still needs measuring on hardware and why that
part is not in this change.

## What the inherited configuration actually does

Two independent things are described in the device tree, and only one of them
can act.

### The control path

`cpu-thermal` is the only zone that can throttle anything.

| Property | Value |
|---|---|
| Sensor | `mediatek,mt8163-thermal`, the SoC junction sensor |
| Reported temperature | the **maximum** of the SoC's five internal sensors |
| Passive trip | 84.904 C, hysteresis 2 C |
| Critical trip | 117.000 C |
| Cooling devices | `cpu0`–`cpu3` via cpufreq, seven OPPs, 600 MHz – 1.3 GHz |
| Thermal governor | `step_wise` (Kconfig default; nothing overrides it) |
| Reaction | polling only, every 1000 ms |

`mtk_thermal.c` fills in exactly one operation:

```c
static const struct thermal_zone_device_ops mtk_thermal_ops = {
	.get_temp = mtk_read_temp,
};
```

There is no `.set_trips`, so the interrupt the sensor node declares is never
used to signal a trip crossing. Polling is the only way a trip is ever noticed,
which makes `polling-delay-passive` the entire reaction time of the system.

### The telemetry path

Three `amazon,virtual_sensor_thermistor` nodes sit on AUXADC channels 0, 12 and
13 and register as zones `mtkts_bts0`–`mtkts_bts2`. They cannot participate in
throttling, and this is structural rather than a matter of configuration:

```c
sensor->tzd = thermal_zone_device_register(zone_name, 0, 0, sensor,
					   &amazon_thermistor_tz_ops,
					   NULL, 0, 0);
```

`trips = 0`, `mask = 0`, `tzp = NULL`, `passive_delay = 0`, `polling_delay = 0`.
No trip points exist to cross and the zones are never polled.

Worse, the number they reported was not a temperature. `get_temp` applied the
vendor virtual-sensor transform:

```
temp = weight/1000 * EMA(raw - offset)
```

With this board's parameters — offset 9 or 10 C, weight 200 — that is
`0.2 * (T - 9 C)`. A thermistor sitting at 70 C reported **12 C**. The value is
a single sensor's weighted *contribution* to a virtual sensor, which the vendor
design sums across the board thermistors and the SoC sensor to estimate
enclosure temperature; the weights only reach unity once the aggregate that
consumes them exists, and no such aggregate exists in this tree.

The result passed every plausibility check anything might apply — it is inside
the `-40..150` range LibreEcho-UI's `read_temperature()` accepts, and the zone
type `mtkts_bts0` matches the `mtk` substring that function looks for. A wrong
number that looks reasonable is worse than an obviously broken one.

This is the reason no enclosure-temperature trip could be set: a trip point on a
sensor reporting `0.2 * (T - 9)` is meaningless. The fix is in this change —
the thermal zone now reports the true temperature off the NCP15XH103 curve,
while the per-device sysfs `temp` and `params` attributes keep the vendor
transform for the tooling that expects that exact number.

## The problems

**1. The two comments did not match the trips they described.** `0x14ba8` is
84.904 C beside a comment reading `85 C`, and `0x1c908` is 117 C beside a comment
reading `115 C`.

The second looked like a transcription slip — exactly one hysteresis step apart.
It is not. Amazon's `thermal.conf` for the same SoC gives its CPU zone an
emergency trip of exactly `117000`, wired to `mtktscpu-sysrst`, above a shutdown
cooler at `100000`. Both values are inherited and correct; only the comments
beside them were wrong. Hand-written hex is why nobody noticed.

**2. One second is a long reaction time when polling is the only path.**
`step_wise` moves exactly one cooling state per poll. With seven OPPs, walking
from 1.3 GHz to 600 MHz took six seconds — and coming back up took six more.
For a device whose entire purpose is answering inside a second, that recovery
cost matters as much as the protection: a brief thermal excursion left the CPU
slow for seconds after the heat was gone.

**3. Nothing protects the temperature a person can feel.** Junction temperature
is the only input to the control loop. The relationship between a 28 nm SoC die
at 85 C and the outside of a sealed plastic cylinder is not fixed — it depends
on ambient temperature and on how long the load has been sustained. The vendor
design addressed exactly this with the virtual sensor; this tree has the
thermistors that would feed it and no zone that consumes them.

**4. The CPU never clocks down on its own.** The defconfig sets
`CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE=y`, so all four cores sit at 1.3 GHz
whenever the thermal governor is not holding them lower. The device spends
almost all of its life idle, waiting for a wake word, and generates heat
throughout at the top of the OPP table. Thermal throttling is the *only*
mechanism that ever lowers the clock.

A side effect worth recording: `ttsd`'s `cpu_boost_begin()` reads the current
governor, skips any CPU already set to `performance`, and so does nothing at all
on this platform. The boost it implements is unreachable because the boosted
state is already the permanent one.

**5. Zone selection in userspace is ambiguous.** `read_temperature()` in
LibreEcho-UI walks `thermal_zone0..15` and returns the first zone whose type
contains `cpu`, `soc`, `mtk` or `thermal`. `mtkts_bts0` contains `mtk`. The
DT-declared `cpu-thermal` zone registers first today so the correct zone wins,
but the tie is broken by probe order rather than by intent.

## The policy in this change

Three device-tree properties, and nothing else — the compiled tree differs in
exactly those three values and is byte-identical everywhere else.

| Property | Was | Now | Why |
|---|---|---|---|
| `polling-delay-passive` | 1000 ms | 250 ms | Polling is the only reaction path. Traverses the OPP range in 1.5 s instead of 6 s, in both directions — and 250 ms is the vendor's own poll interval for this zone. |
| passive trip | 84.904 C | 85.000 C | The value it always claimed. |

The critical trip stays at 117 C. The comment beside it is corrected instead.

`polling-delay` — the interval below every trip, where the device spends nearly
all of its time — stays at 1000 ms. Only the passive interval is shortened, so
the extra register reads happen exclusively while the part is already hot.

Temperatures are now decimal. Both original errors were hex literals that
disagreed with the comment beside them, and decimal removes the step where that
can happen again.

## What is deliberately not here

Three larger changes follow from the evaluation above. Each depends on knowing
the device's real thermal envelope, and none of them should be chosen from
first principles:

- **Enclosure-temperature trips.** The sensors now report real temperatures, so
  trips have become possible; choosing their values has not. It also needs the
  driver converted to `devm_thermal_of_zone_register()` so the zones can be
  described in the device tree with trips and a poll interval. A
  skin-temperature limit picked without measuring the enclosure is a guess with
  a safety label on it, so the values wait for the characterisation below.
- **A different cpufreq governor.** `schedutil` would stop the device idling at
  1.3 GHz, but it trades idle heat against the ramp latency at the start of a
  voice turn, and this device has a sub-second response budget. That trade needs
  the latency measured on both sides, not asserted.
- **Trip values tuned to the real envelope.** 85 C is inherited, not chosen. If
  the part never approaches it in normal use the control loop is decorative; if
  it sits near it under sustained load, the passive trip belongs lower.

### Measuring the envelope

The characterisation these depend on, to be run on hardware:

```sh
# Junction and all three board thermistors, once a second.
while :; do
  printf '%s' "$(date +%s)"
  for z in /sys/class/thermal/thermal_zone*; do
    printf ' %s=%s' "$(cat "$z/type")" "$(cat "$z/temp")"
  done
  printf ' khz=%s\n' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
  sleep 1
done
```

Four numbers decide every open item: the idle steady state, the steady state
under sustained four-core load, how long it takes to get there, and how far the
thermistors track the junction while it happens.
