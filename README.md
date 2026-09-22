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
  own, and makes those settings the one owner of it: a key press writes the
  setting and the LED follows;
- no animation timer. Each pattern reports when it next changes and the redraw
  is scheduled for that moment, and nothing is scheduled at all while the
  keyboard is idle.

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

Brightness 0 is the LED's low-power state rather than a dim one, and a step down
reaches it. The LED is written to zero once, which lets the PWM peripheral stop,
and no redraw is scheduled at all until the brightness rises again; the pattern
then picks up at the point its clock has reached. The split connection indicator
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
animation and the split mirror and writes zero while the LED drivers still
work; the other finds that done. A resume, if the power-off is abandoned,
redraws where the pattern had got to. Their priorities are
`CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY` (45) and
`CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY` (95).

Speed is a percentage of the rate each pattern was drawn at. It applies to the
animation clock rather than to the patterns, so one setting speeds all eighteen
up by the same factor and cannot get a single one of them wrong.

With `CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` a binding does not touch the
LED directly: it writes the setting its behavior is named after, for the
transport the keyboard is on now, and the LED follows that setting - see
[Studio settings](#studio-settings). Without the option, a binding applies its
value to the LED directly.

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

`LED_PATTERN_COUNT` is one past the last. Adding a pattern means raising it and
adding the curve; the cycling commands and the settings range both follow.

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

`idle_off` is per transport for the sharpest version of that argument: USB
means a cable, where keeping the LED lit through idle costs nothing that
matters, and BLE means a battery, where it is the only setting in this module
that really shortens the day.

A pattern is a plain 0 - 17 number rather than a named dropdown because the
Studio RPC schema caps an options constraint at eight values and the handler
clamps to that silently: a list of eighteen would arrive at a client showing
the first eight and losing the other ten. The complete names live where they
fit - in the parameter metadata a keymap editor draws, and in the public
header.

**Key presses write these settings.** The settings are the one owner of the
value. A press writes the setting its behavior is named after, for the live
transport, and the LED follows through the same change event a client's edit
raises, so a client shows the press as it happens and switching transports or
editing another value cannot undo it. The write is in memory at once and saved
to flash three seconds after the last press: a PERSIST write goes to flash with
no debounce of its own, so a run of presses would otherwise be one flash write
per press rather than one per setting.

Only the central registers these. A peripheral needs none of its own: whatever
reaches the central, a key press or an edit made in a client, is mirrored to it
as final state.

With the option off - the default, and what an upstream ZMK build gets - none
of this is compiled and the keymap bindings are the whole interface.

## Indicators

Two things override the selected pattern, because both are more urgent than
decoration. The split indicator wins over the advertising one.

### The split link

On a split the LED is a status display before it is anything else. Both halves
show the same three stages, and neither leaves the indicator until the pair has
actually agreed:

| stage | LED | meaning |
| --- | --- | --- |
| waiting | blink, the shape of pattern 3 | the other half is not there |
| linked | steady, the shape of pattern 0 | the link is up, state not yet agreed |
| synced | the normal pattern, default breathe (1) | the peripheral has confirmed what it is showing |

Five seconds after the link comes up - long enough for the central to have
discovered the peripheral's relay characteristic, because a write before that
is dropped by the transport with nothing but a log line - the central sends a
`led` packet carrying the whole state: pattern, brightness, speed, the
advertising flag, and how far into the current curve it is, so the peripheral
picks the animation up in phase instead of restarting it.

The peripheral answers with a `lea` packet naming the pattern it is now
showing. That acknowledgement is what makes "synced" an observed fact rather
than an assumption, and it is what releases the indicator on both halves. The
connect-time send is made at most five times, half a second apart, waiting for
it and then gives up, leaving the LED on steady - which is the readable
difference between a pair that never linked and a pair that linked but never
agreed. Relay event names are capped at four bytes including the NUL, which is
why both are three characters.

State changes after that send one packet each, coalesced over 20 ms so a
settings apply or a dragged slider is one radio event rather than a stream.

The behaviors are `BEHAVIOR_LOCALITY_CENTRAL`: the central owns the state and
mirrors the result, rather than the peripheral invoking a behavior a second
time of its own. Include the module and the same behavior nodes in both
firmware images.

### The host link

A central with no host overrides the pattern with a 70% pulse for 300 ms
followed by 1.2 seconds fully off, at a fixed rate that does not follow the
speed setting: it says something about the connection, so it has to stay
recognisable whatever the patterns are set to. Selecting a pattern clears it
until the connection state next changes; turning `led.adv_blink` off makes that
permanent. A peripheral has no endpoint of its own and never shows it.

## Scheduling and power

There is no animation tick. Each curve reports the level to show *and* how long
that level stays right, and the redraw is scheduled for exactly that moment: a
blink wakes twice a second rather than forty times, the ten dark seconds at the
end of slow breathe are one sleep, and steady schedules nothing at all. Ramps
are the only shape without edges of their own, and the only thing still sampled
on a clock - at 40 ms, while one is actually being drawn.

The animation also follows `zmk_activity_state_changed`: once ZMK reports the
keyboard idle the LED goes dark and nothing is scheduled until the next press.
Clearing the live `idle_off` buys the other policy - the animation keeps
running for as long as the keyboard is powered - and it is the one setting here
that really does cost battery rather than merely looking as though it might.
Both inputs to that decision are re-read rather than latched, so clearing it
while the keyboard is *already* idle lights the LED straight away instead of
waiting for a key.

**On a split, activity is the central's to decide.** The two halves do not see
the same key presses: ZMK resets activity from `zmk_position_state_changed`,
and a peripheral's presses reach both halves - its own locally, the central's
copy over the split - while a central's reach only the central. A peripheral
left to its own activity therefore goes dark after the idle timeout of someone
typing on the other half, and stays dark. So the mirror carries the central's
activity along with the flag, and a peripheral treats either half's word as
enough: the central's covers typing that never reaches it, its own covers its
keys without waiting for a relay and covers having no central to ask at all.

That costs two extra messages per idle cycle, and each of them carries the
pattern's phase, so waking re-agrees the curve as well as the fact of being
awake.

Both indicators use a true-zero dark phase rather than the 3-5% floor the
blink-family patterns are drawn with. On nRF, Zephyr's PWM driver stops the PWM
peripheral - and lets go of the high-frequency clock it needs - only when every
channel sits at exactly 0% or 100% duty, so a dim floor would keep that clock
running through states that exist because something is wrong and may last a
long time. The advertising indicator's *lit* phase stays at 70%, where the
LED's own current dominates and dimming is the cheaper option.

Every redraw and every mirror runs on `zmk_workqueue_lowprio_work_q()`, never
the system work queue. ZMK posts its advertising-restart work there, and an
animation has no business queuing in front of the item that makes a keyboard
findable again after a host disconnect; separately, Zephyr refuses to block for
an ATT TX buffer when the caller is that queue, so a split write posted from it
fails whenever the buffer pool is momentarily empty.

## Compatibility and tests

The base behaviors build against upstream ZMK `main`. The optional Studio
settings integration additionally needs `cormoran/zmk` `main+dya` and
`zmk-feature-custom-settings`; leaving the option off removes that dependency.

CI builds a real nRF52840 ZMK firmware fixture in both configurations. It checks
that all three behaviors are linked, that the DYA setting namespace and keys
are present only in the DYA build, and that `CONFIG_ZMK_BACKLIGHT` stays off so
there is only one owner of the PWM LED.

## License

[MIT](LICENSE)
