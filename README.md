# ZMK LED Patterns

Reusable ZMK module for a single external LED driven by PWM. It provides the
`zmk,behavior-led-pattern` behavior and, optionally, publishes the LED's whole
state through `zmk-feature-custom-settings` so a Studio client can edit it.

```dts
&led_pattern LED_PATTERN_SLOW_BREATHE
&led_pattern LED_PATTERN_BLINK
&led_pattern LED_PATTERN_STEADY
&led_pattern LED_PATTERN_HEARTBEAT

&led_pattern LED_PATTERN_PREVIOUS
&led_pattern LED_PATTERN_NEXT
&led_pattern LED_PATTERN_BRIGHTNESS_UP
&led_pattern LED_PATTERN_BRIGHTNESS_DOWN

&led_pattern LED_PATTERN_BRIGHTNESS(40)
&led_pattern LED_PATTERN_SPEED(200)
```

For a split keyboard with an LED on both halves, include this module and the
same behavior node in both firmware images. The behavior is
`BEHAVIOR_LOCALITY_CENTRAL`: the central owns the state and mirrors its result
to the peripheral as a small relay-event packet, rather than having the
peripheral invoke the behavior a second time of its own. See
[The split connection indicator](#the-split-connection-indicator).

## The one parameter

One number carries four kinds of command, told apart by which band it falls in:

| band | meaning |
| --- | --- |
| 0 - 17 | select this exact pattern |
| 100 - 103 | previous, next, brighter, dimmer |
| 200 - 300 | `LED_PATTERN_BRIGHTNESS(pct)` - set the brightness to a percentage |
| 410 - 800 | `LED_PATTERN_SPEED(pct)` - set the speed to a percentage |

The public header defines all eighteen pattern constants from
`LED_PATTERN_STEADY` (0) through `LED_PATTERN_FADE_BLINK` (17), plus
`LED_PATTERN_COUNT`.

The two absolute bands exist because a split central has to be able to tell a
peripheral what the state now *is*, rather than how it changed: "next pattern"
and "brighter" only keep two halves together for as long as both started from
the same place. They are ordinary keymap parameters as well.

Speed is a percentage of the rate each pattern was drawn at, from 10 to 400. It
is applied to the animation clock rather than to the patterns, so one setting
speeds all eighteen up by the same factor.

The behavior publishes ZMK parameter metadata, which is what makes it
selectable in a Studio keymap editor: a behavior that publishes none is treated
as one that takes no parameter, and every binding with a non-zero parameter is
rejected as invalid.

## Custom settings

`CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` (needs
`CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC`) registers seven values under the
`amgskobo__led` subsystem, under a fixed `led.` key prefix:

| key | type | range | default |
| --- | --- | --- | --- |
| `led.usb_pattern` | int32 | 0 - 17 | 1, breathe |
| `led.usb_brightness` | int32 | 0 - 100 percent | 100 |
| `led.usb_speed` | int32 | 10 - 400 percent | 100 |
| `led.ble_pattern` | int32 | 0 - 17 | 1, breathe |
| `led.ble_brightness` | int32 | 0 - 100 percent | 100 |
| `led.ble_speed` | int32 | 10 - 400 percent | 100 |
| `led.adv_blink` | bool | show the advertising indicator | true |

A client renders these with no page of its own: the declared type and
constraints drive the widget.

**A whole set per transport.** The live set follows `zmk_endpoint_changed`, so
unplugging USB changes the light instead of changing nothing. Anything that is
not the USB endpoint - a BLE profile, or no endpoint at all while advertising -
reads the BLE set.

Brightness and speed are per transport for the same reason the pattern is: the
two hosts are not looked at under the same conditions. A keyboard on USB is on
a desk in a lit room; the same keyboard on BLE is as likely to be somewhere
dark, where the number that reads as "on" is a different one. The advertising
indicator is the one value that is not split, because it is about having no
connection at all rather than about which one.

A pattern is a plain 0 - 17 number rather than a named dropdown because the
Studio RPC schema caps an options constraint at eight values and the handler
clamps to that silently, so a list of eighteen would arrive at a client showing
the first eight and losing the other ten. The complete names live where they
fit: in the behavior's parameter metadata, which a keymap editor draws, and in
the public header.

Keymap bindings keep working with this on. The settings are the owner of the
value: a press changes the LED there and then, and the next settings event puts
the stored set back. Nothing is written back from a key press, because a value
with two owners is how the two views come to disagree.

This file is deliberately the same shape as the sibling modules
(`zmk-input-vector-acceleration`, `zmk-input-processors`): register the
subsystem, register the values, listen, apply. No work item, no poll, no gate
of its own, and no private split relay.

Only the central registers these. A peripheral needs no settings of its own:
whatever reaches the central - a key press or a client's edit - is mirrored to
it as final state, so the two cannot hold different opinions about the same
value.

With `CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS` off, which is the default and
what an upstream ZMK build gets, none of that is compiled and the keymap
bindings are the whole interface.

## The split connection indicator

On a split, the LED is a status display before it is anything else. Both halves
show the same three stages and neither leaves the indicator until the pair has
actually agreed:

| stage | LED | meaning |
| --- | --- | --- |
| waiting | blink, the shape of pattern 3 | the other half is not there |
| linked | steady, the shape of pattern 0 | the BLE link is up, state not yet agreed |
| synced | the normal pattern, default breathe (1) | the peripheral has confirmed what it is showing |

Three seconds after the link comes up - long enough for the central to have
discovered the peripheral's relay characteristic, because a write before that
is dropped by the transport - the central sends a `led` packet carrying the
whole state: pattern, brightness, speed, the advertising flag, and how far into
the current curve it is, so the peripheral picks the animation up in phase
instead of restarting it.

The peripheral answers with a `lea` packet naming the pattern it is now
showing. That acknowledgement is what makes "synced" an observed fact rather
than an assumption, and it is what releases the indicator on both halves. The
connect-time send retries at most four times over two seconds waiting for it
and then stops, leaving the LED on steady - which is the readable difference
between a pair that never linked and a pair that linked but never agreed.
Relay event names are capped at four bytes including the NUL, which is why both
are three characters.

State changes after that send one packet each, coalesced over 20 ms so that a
settings apply or a dragged slider is one radio event rather than a stream.

## Scheduling and power

There is no animation tick. Each curve reports the level to show *and* how long
that level stays right, and the redraw is scheduled for exactly that moment: a
blink wakes twice a second rather than forty times, the ten dark seconds at the
end of slow breathe are one sleep, and steady schedules nothing at all. Ramps
are the only shape without edges of their own and are sampled at 40 ms while
they are actually being drawn.

The animation also follows `zmk_activity_state_changed`: once ZMK reports the
keyboard idle the LED goes dark and nothing is scheduled until the next press.

Both indicators use a true-zero dark phase. `pwm_nrfx_set_cycles()` stops the
PWM peripheral - and releases the high-frequency clock it needs - only when
every channel sits at exactly 0% or 100% duty, so a 3-5% "off" floor would keep
that clock running through states that exist because something is wrong and may
last a long time. The advertising indicator's *lit* phase stays at 70%, where
the LED's own current dominates and dimming is the cheaper option.

Every redraw and every mirror runs on `zmk_workqueue_lowprio_work_q()`, never
the system work queue: ZMK posts its advertising-restart work there, and
Zephyr refuses to block for an ATT TX buffer when the caller is that queue.

## The advertising indicator

An unconnected central overrides the pattern with a 70% pulse for 300 ms
followed by 1.2 seconds fully off, at a fixed rate that does not follow the
speed setting: it says something about the connection, so it has to stay
recognisable. Selecting a pattern clears it until the connection state next
changes; switching `adv_blink` off makes that permanent. A peripheral has no
endpoint of its own and never shows it.

This is about the *host* link. The split connection indicator above is about
the other half, and it wins while it is showing.

The default connected pattern is breathe.

## Use from another ZMK config

Add it as a project in the consuming config's `config/west.yml`:

```yaml
  remotes:
    - name: amgskobo
      url-base: https://github.com/amgskobo
  projects:
    - name: zmk-led-patterns
      remote: amgskobo
      revision: main
```

The consuming keymap needs the behavior node and the public header:

```dts
#include <dt-bindings/zmk/led_pattern.h>

/ {
    behaviors {
        led_pattern: led_pattern {
            compatible = "zmk,behavior-led-pattern";
            #binding-cells = <1>;
        };
    };
};
```

The board must also provide a `zmk,backlight` chosen node backed by one PWM
LED, because the module controls LED child index zero of that device.

The settings key does not depend on the behavior node's name, so renaming
`led_pattern` in a keymap does not orphan the stored values. Its prefix is
fixed because the module controls one LED - the one behind the `zmk,backlight`
chosen node, which devicetree allows exactly one of - so there is nothing to
tell apart.
