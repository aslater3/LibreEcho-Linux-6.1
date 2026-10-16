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
## The measured envelope

Taken on hardware over ADB: 60 s idle, 300 s of four-core load, 120 s cooldown,
sampled every 2 s.

| | Junction | Notes |
|---|---|---|
| Idle | **66.7 C** mean (65.3–71.8) | CPU 100 % idle, all four cores pinned at 1.3 GHz |
| Sustained four-core load | **81.5 C** steady, 81.9 peak | reached 90 % of the rise by t+150 s |
| Cooldown | 81.4 → 68.8 C in 120 s | |
| Board thermistor `bts0` | 22.1 → 30.0 C | true value, transform inverted |

**The passive trip never fired.** `cooling_device0/cur_state` stayed at 0 for the
entire run and the cores never left 1.3 GHz. Peak came 3.1 C short of the 85 C
trip, so on this workload the control loop is decorative.

The idle figure is the striking one: 66.7 C with the CPU 100 % idle. Nothing is
computing — that is the cost of holding four cores at their top OPP.

## What the vendor does, for comparison

From a stock Echo Dot 2nd Gen dump (`biscuit`, same MT8163, different board).
`thermal.policy.conf` is plaintext; `thermal.conf` is obfuscated with a
per-line `c[i] - (i mod 10)` shift and decodes cleanly.

The kernel zone ladder, `/proc/driver/thermal/tzcpu`:

| Trip | Cooler |
|---|---|
| 117000 | `mtktscpu-sysrst` — emergency reset |
| 100000 | `mtk-cl-shutdown00` |
| 95000 | `cpu02` |
| 90000 | `cpu_adaptive_0` |
| 76000 | `cpu_adaptive_1` |
| **67000** | `cpu_adaptive_2` |

Poll interval: **250**.

Two things fall out of this. The 117 C critical trip in our tree is not a
transcription error — it is exactly the vendor's emergency trip. And 250 ms,
arrived at here from the step_wise reasoning above, is exactly the vendor's poll
interval for the same zone.

Above that sits a second layer this tree has no equivalent of: `ace_thermald`
running `thermal.policy.conf`, which throttles on **skin temperature** from a
`tmp103` sensor across seven trips from 56.5 C to 60.0 C, allocating a power
budget (2950 → 251 mW) rather than capping frequency.

### What that comparison implies

Measured against the vendor's own ladder, our device would have been throttling
for **92 %** of the run at the 67 C trip and **62 %** at the 76 C trip. It
throttled for 0 % of it. Even at idle, 66.7 C sits essentially on the vendor's
first throttle point.

Two caveats keep this from being a straight verdict. `biscuit` is a Dot — a puck
with far less thermal mass than this cylinder — and its aggressive band is partly
about the skin temperature of something people pick up. And its coolers allocate
a power budget, which degrades far more gently than walking the OPP table.

So the vendor numbers are not values to copy. What they establish is that 85 C
as the single throttle point is far more permissive than the people who designed
the hardware intended, and that the gap is large enough to be deliberate rather
than incidental.

## What mainline does, and what this tree now mirrors

MT8163 was never upstreamed — mainline 6.1 has no `mt8163.dtsi`. Its closest
relatives are `mt8173` (same era, same V1 thermal controller in `mtk_thermal.c`)
and `mt8183`. Both describe their CPU zone the same way:

| | mt8173 | mt8183 | vendor `biscuit` | this tree, before |
|---|---|---|---|---|
| first response | `threshold` **68000** | `threshold` **68000** | `cpu_adaptive_2` **67000** | — |
| control target | `target` 85000 | `target` 80000 | ladder to 95000 | single trip 84904 |
| critical | 115000 | 115000 | **117000** | 117000 |
| passive poll | 1000 | 100 | **250** | 1000 |
| power model | `sustainable-power`, `dynamic-power-coefficient` | same | `thermal_budget` in mW | none |

That table resolves the trip-comment mystery completely. **115000 is mainline's
critical and 117000 is the vendor's.** Whoever wrote this zone took mainline's
structure — hence a comment reading `115 C` — and the vendor's value. Both
numbers were real; they came from different places.

It also shows what was dropped. Mainline's zone has *two* passive trips, and the
naming is not decorative: `threshold` is where control begins and `target` is
the temperature the governor regulates toward, which is the `power_allocator`
(IPA) contract. Along with `sustainable-power` and `contribution`, mainline's
zone is built for a power-budget governor — the same approach the vendor takes
with `thermal_budget` and its `cpu_adaptive_*` coolers in milliwatts. This tree
kept `target`, dropped `threshold`, dropped the power model, and ran step_wise.

### The mirror

- **`dynamic-power-coefficient = <263>`** on all four CPUs — mainline's figure
  for the mt8173's A53 cluster, the same core on the same process generation.
  It cross-checks against Amazon: 263 uW/MHz/V² puts four cores at 1216 MHz on
  1.25 V at 1999 mW, and the vendor's second `thermal_budget` step is 1958 mW.
- **`threshold` at 68000**, unmapped, exactly as mainline leaves it.
- **`target` at 80000**, carrying the cooling map — mt8183's figure, and just
  under the 81.5 C this board reaches on sustained load. It is the first trip in
  this tree that will actually engage.
- **`sustainable-power = <2000>`**, a little under the ~2311 mW four cores draw
  at 1.3 GHz. Needs confirming on hardware.
- `CONFIG_ENERGY_MODEL` and `CONFIG_THERMAL_GOV_POWER_ALLOCATOR`, in a new
  `libreecho_mt8163_thermal.config`.

### Why the default governor is deliberately left alone

`thermal_zone_device_register()` treats a failed governor bind as fatal to the
zone — it jumps straight to `unregister`. `power_allocator_bind()` calls
`check_power_actors()`, which fails unless every cooling device attached to the
zone exposes a power model. So making IPA the default means that if the energy
model does not come up for any reason, the whole `cpu-thermal` zone disappears,
taking the critical trip with it. That is worse than the problem being solved,
and it cannot be verified without booting the result.

step_wise therefore stays the default. It still throttles at `target`, which is
already an improvement on a trip that never fired, and `threshold` stays
unmapped so it cannot cap the clock at idle — the device idles at 66.7 C, astride
that trip. Switching one zone is reversible at runtime and a failed switch falls
back rather than unregistering:

```sh
echo power_allocator > /sys/class/thermal/thermal_zone0/policy
```

Flipping the compiled default is a one-line change to the fragment once that has
been confirmed on hardware.

## Still open

- **Confirming the mirror on hardware.** None of it has been booted.
  `sustainable-power` in particular is an estimate from the power model and one
  characterisation run, and IPA's behaviour at `threshold` is the whole reason
  68 C is safe to declare — both need watching on a real device before the
  default governor is flipped.
- **A different cpufreq governor.** Worth more than the trip values --- 66.7 C at
  100 % idle is paid continuously for nothing. But `scaling_available_governors`
  reads `performance` and nothing else: no other governor is compiled in, so
  this is a kernel config change and a reflash, not a sysfs write. `ttsd`'s
  `cpu_boost_begin()` is doubly dead as a result --- the boosted state is already
  permanent, and there is no other governor it could have come from.
- **Enclosure-temperature protection.** The sensors report real temperatures as
  of this change, so trips are now possible. Two of the three thermistors read
  -19.0 C and are almost certainly unpopulated; `bts0` tracks ambient and rose
  only 8 C across a run that moved the junction 15 C, so it is a poor proxy for
  the die and a reasonable one for the room. There is no `tmp103` on this board.

### Reproducing the measurement

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

Write only to `/tmp`. `/data` has a strict allowlist and an unexpected file
there halts every service on the next boot, in both A/B slots.
