# ZMK LED Patterns

[![Test](https://github.com/amgskobo/zmk-led-patterns/actions/workflows/test.yml/badge.svg)](https://github.com/amgskobo/zmk-led-patterns/actions/workflows/test.yml)

[日本語](README_JA.md)

Eighteen animated patterns for one external LED, as ZMK behaviors. On a split
keyboard it doubles as the connection indicator: the LED says whether the two
halves have found each other before it says anything else.

- one keymap behavior per setting - `&led_pattern`, `&led_brightness` and
  `&led_speed` - each with parameter metadata so a Studio keymap editor can
  bind it;
- optionally publishes the whole LED state through
  `zmk-feature-custom-settings`, so a client can edit it with no page of its
  own. BLE and USB key presses update the setting immediately; USB coalesces
  Studio notification dispatch for rapid key changes;
- no fixed animation tick. Each pattern reports its next change. With idle-off
  enabled, normal animation redraw stops while idle; foreground settings,
  connection and power transitions retain their own bounded work.

## Install

### 1. Add the module

In the consuming config's `config/west.yml`:

```yaml
  remotes:
    - name: amgskobo
      url-base: https://github.com/amgskobo
  projects:
    - name: zmk-led-patterns
      remote: amgskobo
      revision: main
```

### 2. Point it at an LED

The module drives child index 0 of the `zmk,backlight` chosen node, so the
board or shield has to put a PWM-backed LED there:

```dts
/ {
    chosen {
        zmk,backlight = &backlight;
    };

    backlight: pwmleds {
        compatible = "pwm-leds";

        pwm_led_0 {
            pwms = <&pwm0 0 PWM_MSEC(10) PWM_POLARITY_NORMAL>;
        };
    };
};
```

and a PWM driver for it (`CONFIG_PWM=y`; Zephyr then enables `LED_PWM` on its
own for a `pwm-leds` node). The module selects `LED` itself. The chosen node is
only how it finds the LED, so ZMK's backlight subsystem is not needed and is
best left off: with `CONFIG_ZMK_BACKLIGHT=y`, `backlight.c` also writes that LED
at boot and `&bl` becomes a second owner of it.

### 3. Declare the behaviors

```dts
#include <dt-bindings/zmk/led_pattern.h>

/ {
    behaviors {
        led_pattern: led_pattern {
            compatible = "zmk,behavior-led-pattern";
            #binding-cells = <1>;
        };

        led_brightness: led_brightness {
            compatible = "zmk,behavior-led-brightness";
            #binding-cells = <1>;
        };

        led_speed: led_speed {
            compatible = "zmk,behavior-led-speed";
            #binding-cells = <1>;
        };
    };
};
```

The `led_pattern` node is required: it owns the LED controller the other two
drive, and the build fails with a message if one of them is declared without
it. The brightness and speed nodes are optional. The nodes may be called
anything: no setting key is derived from their names, so renaming one later
does not orphan stored values. Declare no more than one node of each compatible;
the build rejects duplicates instead of silently registering only the first.

### Options

| symbol | default | |
| --- | --- | --- |
| `CONFIG_ZMK_LED_PATTERNS` | on when a `zmk,behavior-led-pattern` node exists | the behaviors and the animation. Selects `LED`. |
| `CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS` | `n` | publish the state for a client to edit, and have key presses write it. Needs `ZMK_CUSTOM_SETTINGS_STUDIO_RPC`. |

The module selects `LED` and `ZMK_LOW_PRIORITY_WORK_QUEUE`, and
`ZMK_SPLIT_RELAY_EVENT` on a split build.

## The behaviors

One behavior per setting, each taking one parameter:

| behavior | parameter | |
| --- | --- | --- |
| `&led_pattern` | 0 - 17 | select this exact pattern |
| | `LED_PATTERN_PREVIOUS`, `LED_PATTERN_NEXT` | step to the previous or next pattern, wrapping around |
| `&led_brightness` | 0 - 100 | set this brightness percentage |
| | `LED_BRIGHTNESS_DOWN`, `LED_BRIGHTNESS_UP` | ten points down or up, between 0 and 100 |
| `&led_speed` | 10 - 400 | set this speed percentage |
| | `LED_SPEED_DOWN`, `LED_SPEED_UP` | ten points down or up, between 10 and 400 |
| | `LED_SPEED_MIN`, `LED_SPEED_DEFAULT`, `LED_SPEED_MAX` | 10, 100 or 400 percent |

```dts
&led_pattern LED_PATTERN_HEARTBEAT
&led_pattern LED_PATTERN_NEXT
&led_brightness LED_BRIGHTNESS_DOWN
&led_brightness 40
&led_speed 200
&led_speed LED_SPEED_UP
&led_speed LED_SPEED_DEFAULT
```

A step lands on a multiple of ten, so a brightness of 95 set from a client steps
to 100 or 90 rather than carrying the 5 along, and a speed step does the same
between its bounds.

Brightness 0 requests zero from the LED driver and stops normal animation
redraw until brightness rises again. The driver/PWM hardware determines the
actual electrical and current response. Split quality expiry is consumed once;
it does not start background LED synchronization. When brightness rises, a
reusable fresh clock can preserve phase; a stale clock or refit deliberately
starts a new phase at a validated future common edge. The split connection indicator
is the one thing that still lights at zero, because it is a diagnostic and has to
look the same whatever the LED is set to. The advertising indicator is scaled by
the brightness like the patterns, so it goes dark too.

A power-off writes the LED dark as well. ZMK's `&soft_off` and its idle sleep
raise no event, and a pin keeps its level for as long as the SoC is off: an LED
lit at that moment stayed lit through soft-off - and, through a transistor, a
gate left charged by a released pin does the same. With `CONFIG_PM_DEVICE` (soft
off selects it) the module registers two small power-management devices, one
before the PWM driver and one after the LED device in init order, because the
two power-off paths suspend in opposite orders. Whichever runs first stops the
animation and fences split work, then writes zero before waiting for transport
work to drain. The other finds that done. An abandoned split suspend starts a
new clock handshake and future reservation; standalone redraws its local curve. Their priorities are
`CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY` (45) and
`CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY` (95).

Speed is a percentage of the rate each pattern was drawn at. It applies to the
animation clock rather than to the patterns, so one setting speeds all eighteen
up by the same factor and cannot get a single one of them wrong.

With `CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` a binding does not touch the
LED directly: it writes the setting its behavior is named after, for the
transport the keyboard is on now, and the LED follows that setting - see
[Studio settings](#studio-settings). Without the option, a binding requests the
controller directly. Standalone output follows immediately; split output uses
the shared future reservation in either path.

Each behavior publishes ZMK parameter metadata, which is what makes it
selectable in a Studio keymap editor. A behavior that publishes none is treated
as one that takes no parameter at all, and every binding whose parameter is
non-zero is rejected as invalid.

### The eighteen patterns

| | constant | | constant |
| --- | --- | --- | --- |
| 0 | `LED_PATTERN_STEADY` | 9 | `LED_PATTERN_BEACON` |
| 1 | `LED_PATTERN_BREATHE` | 10 | `LED_PATTERN_SLOW_BREATHE` |
| 2 | `LED_PATTERN_HEARTBEAT` | 11 | `LED_PATTERN_STROBE` |
| 3 | `LED_PATTERN_BLINK` | 12 | `LED_PATTERN_DOUBLE_BEACON` |
| 4 | `LED_PATTERN_FAST_BLINK` | 13 | `LED_PATTERN_LONG_FLASH` |
| 5 | `LED_PATTERN_TRIPLE_FLASH` | 14 | `LED_PATTERN_SPARKLE` |
| 6 | `LED_PATTERN_SOS` | 15 | `LED_PATTERN_COUNTDOWN` |
| 7 | `LED_PATTERN_CANDLE` | 16 | `LED_PATTERN_RIPPLE` |
| 8 | `LED_PATTERN_SAWTOOTH` | 17 | `LED_PATTERN_FADE_BLINK` |

`LED_PATTERN_COUNT` is one past the last. Raising it updates the cycling commands
and settings range. Adding a pattern also requires its curve and an updated split
protocol range, plus a paired wire schema/version update when needed. Recalculate
the period LCM used for overflow-safe phase scaling, update the curve/runtime,
codec and 64-bit phase tests, and run the kernel checks if scheduling or the wire
contract changes.

## Studio settings

`CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` registers nine values under the
`amgskobo__led` subsystem with a fixed `led.` key prefix:

| key | type | range | default |
| --- | --- | --- | --- |
| `led.usb_pattern` | int32 | 0 - 17 | 1, breathe |
| `led.usb_brightness` | int32 | 0 - 100 percent | 100 |
| `led.usb_speed` | int32 | 10 - 400 percent | 100 |
| `led.usb_idle_off` | bool | go dark once the keyboard is idle | true |
| `led.ble_pattern` | int32 | 0 - 17 | 1, breathe |
| `led.ble_brightness` | int32 | 0 - 100 percent | 100 |
| `led.ble_speed` | int32 | 10 - 400 percent | 100 |
| `led.ble_idle_off` | bool | go dark once the keyboard is idle | true |
| `led.adv_blink` | bool | show the advertising indicator | true |

A client renders these with no page of its own: the declared type and
constraints drive the widget.

**A whole set per transport.** The live set follows `zmk_endpoint_changed`, so
unplugging USB changes the light instead of changing nothing. Anything that is
not the USB endpoint - a BLE profile, or no endpoint at all while advertising -
reads the BLE set. Brightness and speed are per transport for the same reason
the pattern is: a keyboard on USB is on a desk in a lit room, and the same
keyboard on BLE is as likely to be somewhere dark, where the number that reads
as "on" is a different one. The advertising indicator is the one value that is
not split, because it is about having no connection at all rather than about
which one.

`idle_off` is per transport so a cable-powered USB session can keep animating
while a BLE session can stop normal animation when idle. Brightness, pattern,
LED hardware and radio activity also affect power. This module has no measured
battery-life or current-consumption guarantee.

A pattern is a plain 0 - 17 number rather than a named dropdown because the
Studio RPC schema caps an options constraint at eight values and the handler
clamps to that silently: a list of eighteen would arrive at a client showing
the first eight and losing the other ten. The complete names live where they
fit - in the parameter metadata a keymap editor draws, and in the public
header.

**Key presses update the live registry and desired getters immediately on both
transports.** Standalone output follows the controller immediately. Split output
changes at a shared future reservation; repeated key steps use the latest desired
value while that reservation is pending. USB combines notification attempts after 300 ms of quiet. Each
attempt reads the current registry value; it never replays an older pending
key value over a later client edit. Settings are saved to flash three seconds
after the last memory write, using the registry's atomic save of its current
value. A client edit updates desired state immediately. A failed memory write is
reported and the behavior falls back to the controller request, with the same
standalone or split presentation policy; it creates no pending registry value
or dirty save for that failed write.

USB-key writes defer only their Studio notifications while core observers see
the change immediately. Saves suppress Studio notifications while the USB
Studio CDC port is closed. Current custom-settings supports call-scoped quiet
events, so an unrelated concurrent write retains its notification. The original
supported c6a7 dependency uses its existing global suppression bracket and
retains that dependency's concurrent-suppression limitation. Reconnecting
clients can read the registry. Event dispatch success does not acknowledge the
Studio queue, BLE/USB delivery or congestion recovery.

Only the central registers these. A peripheral needs none of its own: whatever
reaches the central, a key press or an edit made in a client, is scheduled on both halves as final state.

With the option off - the default, and what an upstream ZMK build gets - none
of this is compiled and the keymap bindings are the whole interface.

## Indicators

Two things override the selected pattern, because both are more urgent than
decoration. The split indicator wins over the advertising one.

### The split link

The central synchronizes an LED-only logical clock with its one peripheral,
then both halves reserve the same future central timestamp. Receiving a state
packet does not apply it immediately. The visible phase comes from a shared
64-bit epoch. Settings and desired getters still update immediately, so a held
step uses the latest requested value. One immutable reservation and one latest
desired snapshot keep continuous edits bounded.

| stage | LED | meaning |
| --- | --- | --- |
| WAITING | fixed waiting blink | no peer link |
| LINKED | steady diagnostic | no usable completed reservation |
| RUNNING_UNCONFIRMED | normal reserved pattern | output confirmation is pending |
| SYNCED | normal reserved pattern | required completion confirmed, current clock quality bounded |
| COASTING | last successful pattern, free-running | confirmed output with expired/unbounded phase accuracy |
| COASTING_UNCONFIRMED | last successful pattern, free-running | phase accuracy and complete output confirmation are absent |

Clock exchange starts five seconds after link-up. Version4 `ls4` messages carry
both boot/link identities and sequence numbers; exact complete-state ACKs cannot
confirm another reservation. The four-message clock exchange accounts for queue
delay and path asymmetry, rejects network RTT above80ms, and uses an explicit
50ms modeled uncertainty limit and8s expiry. Future apply is1000+2RTTms ahead;
quality is checked at that future time and actual completion. These are software
admission/recovery bounds, not measured physical phase accuracy.

Clock exchange runs on demand at startup, reconnect, resume and effective visual
changes. Settings can reuse an acknowledged fit only while the future reservation
still passes the same bounds. There is no periodic clock/STATE traffic or endless
recovery loop. One transaction permits five clock probes and three future plans,
each with at most three admission copies; coalesced edits do not refill it. A new
latest intent can receive its own finite budget when the old transaction ends.
An exact ACCEPTED stops redundant copies without confirming output.

After accuracy expires, the last successful pattern keeps free-running as
COASTING or COASTING_UNCONFIRMED. A later stale-to-fresh update deliberately
restarts a common phase at the future edge. Failed new output restores and
replays the prior successful view. ACK loss is not atomic delivery. Both halves
require matching version4; periodic version3/ls3 and earlier protocols are incompatible.

The behaviors remain `BEHAVIOR_LOCALITY_CENTRAL`. Include the module and the same
behavior nodes in both images. See [the complete timing and wire contract](SCHEDULED_SYNC.md)
for quality math, session fencing, PM recovery, packet/stack cost and evidence
limits. Physical BLE/LED acceptance remains a separate check.

### The host link

A central with no host overrides the pattern with a 70% pulse for 300 ms
followed by 1.2 seconds fully off, at a fixed rate that does not follow the
speed setting: it says something about the connection, so it has to stay
recognisable whatever the patterns are set to. Selecting a pattern clears it
until the connection state next changes; turning `led.adv_blink` off makes that
permanent. A peripheral has no endpoint of its own; it displays the central's
reserved advertising flag together with the other shared presentation fields.

## Scheduling and power

There is no animation tick. Each curve reports the level to show *and* how long
that level stays right, and the redraw is scheduled for exactly that moment: a
blink wakes twice a second rather than forty times, the ten dark seconds at the
end of slow breathe are one sleep, and steady schedules nothing at all. Ramps
are the only shape without edges of their own, and the only thing still sampled
on a clock - at 40 ms, while one is actually being drawn.

The animation also follows `zmk_activity_state_changed`: once ZMK reports the
keyboard idle the LED goes dark and animation redraw stops until the next press.
The next effective split activity change is reserved; expiry alone does not send
LED traffic. With idle-off disabled, activity changes do not start LED transactions.
Clearing the live `idle_off` buys the other policy - the animation keeps
running for as long as the keyboard is powered - and it keeps animation work active. Event-driven split transactions have their
own bounded cost.
Both inputs to that decision are re-read rather than latched, so clearing it
while the keyboard is *already* idle requests light without waiting for a key;
a split uses its next future reservation.

**On a split, normal activity is central-authoritative.** The central sees
positions from both halves and reserves activity/idle policy changes together
with the presentation state. A wake resets the shared phase at a future edge.
Local activity controls the diagnostic indicator when no usable reservation is
running. PM still darkens the local LED immediately through its separate fence
and drain sequence.

Both indicators use a true-zero dark phase rather than the 3-5% floor the
blink-family patterns are drawn with. On nRF, Zephyr's PWM driver stops the PWM
peripheral - and lets go of the high-frequency clock it needs - only when every
channel sits at exactly 0% or 100% duty, so a dim floor would keep that clock
running through states that exist because something is wrong and may last a
long time. The advertising indicator's *lit* phase stays at 70% duty. Its
total current, including PWM and clock overhead, requires measurement on the
target board.

Standalone redraw and split TX use ZMK's low-priority queue. Split redraw and
reserved apply share a separate1536-byte queue at the same priority, so a core
relay enqueue that blocks cannot occupy the render queue. Neither uses the
system queue that restarts host advertising. Runtime stack high-water, physical
scheduling accuracy and power have not been inferred from compilation.

## Optional battery ADC load correction

Boards whose LED load makes the battery ADC read low can wrap their existing
`zmk,battery-voltage-divider` without changing ZMK's battery code. For a
measured 50 mV drop at 100% LED duty, add this node and select it as
`zmk,battery`:

```dts
/ {
    chosen { zmk,battery = &led_compensated_battery; };
    led_compensated_battery: led_compensated_battery {
        compatible = "zmk,led-battery-adc-offset";
        source-sensor = <&vbatt>;
        full-duty-offset-mv = <50>;
    };
};
```

After the divider fetch, the wrapper reads the duty recorded by the latest
successful local LED write and adds `round(50 mV * duty / 100)`. PWM changes
are not latched to ADC acquisition, so this is a load-correction estimate.
A transition during acquisition can make the recorded duty differ from the
sampled load; electrical accuracy requires board measurements. A recorded
zero duty adds zero. Temporary connection indicators use their recorded duty.
ZMK reports the state of charge,
which the divider computes from its own, uncorrected voltage, so the wrapper
maps the corrected voltage to a percentage again exactly as the divider does:
over the divider's `mv-to-pct-thresholds` on the DYA fork, read from the same
node, and with ZMK's fixed lithium-ion line (0% at 3450 mV, 100% at 4200 mV)
on upstream ZMK, whose divider has no thresholds. It serves both
`SENSOR_CHAN_GAUGE_VOLTAGE` and
`SENSOR_CHAN_GAUGE_STATE_OF_CHARGE`. BAS, the split peripheral's level and any
display then see the corrected value. The correction stacks on the divider
resistor values, which a board may keep as its fixed base calibration. Both
halves need the node if both have the same LED/ADC coupling.
This linear model is a starting calibration based on measured full-duty sag;
verify the sign and intermediate duties on the actual hardware before flashing.

## Compatibility and tests

| Core and role | Supported path |
| --- | --- |
| Stock ZMK, standalone | Base behaviors and optional battery correction |
| DYA, standalone | Base behaviors; optional custom-settings/Studio integration |
| DYA, split central and one peripheral | Scheduled synchronization through the DYA relay API; optional settings on central |
| Stock ZMK, split | Not applicable: the required DYA relay API is absent |

The integration manifests track upstream `main` and `cormoran/zmk` `main+dya`
for compatibility checks. The default DYA fixture pins its legacy registry; an
explicit current-registry override is an additional interface check. See the
[owner test guide](tests/README.md) for exact offline pins and commands.

CI builds four nRF52840 fixtures: stock standalone, DYA standalone, DYA central
and DYA peripheral. It also runs both roles against the pinned Zephyr native
kernel with controlled relay/PWM APIs. Firmware validation checks all three
behaviors and the DYA setting namespace and nine keys where settings are enabled, that `CONFIG_ZMK_BACKLIGHT` stays off so
there is only one owner of the PWM LED, and that ZMK's battery reporting, in its
state-of-charge fetch mode, reads the linked battery wrapper as its chosen
`zmk,battery`.

The host regression suite runs with ASan/UBSan via
`bash ./tests/run-host-docker.sh`; full mode also retains optimized and coverage
variants. It covers immediate registry values, independent fields, later client
edits, updates during notification dispatch, controlled dispatch failure/retry,
atomic delayed saves, synchronous memory failure/recovery, and USB port-open
decisions through both current and original registry APIs. A deterministic
100,000-step interleaving test retains the original generation/dispatch input
schedule; CI requires
100% line and branch coverage of the small `usb_pending.h` helper. The host
suite also checks the ADC offset arithmetic, compares both percentage mappings
with ZMK's own code (the float interpolation and the lithium-ion line) at every
millivolt from 0 to 5 V, and fault-injects the actual battery sensor callbacks
against a stand-in that, like the real divider, refuses anything but the gauge
channels. The host harnesses exercise selected controller, settings bridge,
battery and behavior functions, including the eighteen curves, standalone BLE
and non-BLE roles, and the split controller API. Full mode enforces the documented
line and branch coverage gates for small helpers and selected runtime functions.
Native tests separately exercise the actual scheduled adapter and kernel
interleavings. Firmware fixtures cover devicetree instantiation, relay
registration and settings declarations.

See [the module-owned test guide](tests/README.md) for current commands,
assertion replacement maps and physical acceptance requirements.

The USB mitigation applies
to this module's LED-originated setting notifications. It does not change the
shared DYA RPC transmitter or guarantee that unrelated Studio producers cannot
block if its transmit buffer fills. A successful local event dispatch is not
feedback from the Studio queue or the physical transport.

## Upgrading a split from ls3 to ls4

1. Select the same module revision for both halves and build both firmware images
   against a supported DYA core. Keep that revision in the consuming manifest for
   a reproducible deployment.
2. Update both halves as a pair. Version4/`ls4` deliberately rejects version3/`ls3`
   and older peers; a temporary mixed pair cannot synchronize its normal display.
   There is no automatic wire fallback.
3. Reconnect and check startup, repeated settings, idle/wake and independent
   suspend/resume on both halves. Existing LED keys/settings require no migration
   or settings/bond reset for this wire change.

Desired settings remain immediate, while visible split changes use a future
common edge. Between meaningful events the display free-runs in COASTING after
clock expiry; long-term physical phase alignment is not guaranteed. See
[SCHEDULED_SYNC.md](SCHEDULED_SYNC.md) for finite recovery and freshness rules.

Before a release, test matching production boards for actual split/BLE delivery,
congestion/reconnect, PWM timing, power, USB/Studio coexistence and reset/PM
retention. Host/native tests and firmware builds do not satisfy those physical
acceptance gates.

## License

[MIT](LICENSE)
