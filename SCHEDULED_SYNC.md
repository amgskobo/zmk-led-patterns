# Scheduled split LED presentation

The central is the LED time authority. The module maintains a logical mapping
between each half's uptime; it never changes Zephyr uptime or another module's
clock. Both halves reserve the same future central timestamp and derive normal
pattern and advertising phase from the same central epoch. Receipt of a state
packet does not immediately change the visible LED.

Desired settings and public getters update immediately, including repeated key
steps. A split keeps one immutable reservation and one latest desired snapshot.
New edits do not postpone the already armed edge. The next reservation captures
the latest values. Whole-state setters remain whole-state operations; direct
pattern/brightness/speed requests update only their field under the metadata
mutex. Applying a visible snapshot never writes the settings registry or emits
an older desired-state event. The settings bridge serializes complete registry
read/application callbacks so an older overlapping callback cannot finish last
with an obsolete snapshot.

## Clock exchange and admission

An `ls4` version4 exchange has four messages:

1. Central captures `t1` immediately before raising PROBE.
2. Peripheral captures `t2` on listener entry and `t3` immediately before REPLY.
3. Central captures `t4` on listener entry, validates the sample, and sends FIT.
4. Peripheral independently validates the same sample and echoes FIT_ACK.

Offset is peripheral time minus central time. Its interval is
`[t3 - t4, t2 - t1]`; network RTT is `(t4 - t1) - (t3 - t2)`. This interval
includes asymmetric paths and core queue residence. A small RTT does not prove
symmetry. Accepted samples require network RTT at most80ms and total exchange
at most500ms. Integer uptime timestamps receive the conservative quantization
allowance `2 * max(1, ceil(1000 / ticks_per_second))` milliseconds. An additional
declared1000ppm relative aging budget is a software model assumption, not a
measured oscillator guarantee.

The midpoint mapping has base error `ceil(RTT/2) + quantization + exchange
aging`. Admission and completion require total modeled error at most50ms and
sample age below8000ms. Both are evaluated at the future apply timestamp, not
only at receipt. Clamp error after a backward mapping change is included in
the same budget. A sample can become unusable before8000ms.

Reservations use a lead of `1000 + 2 * measured_RTT` milliseconds. A peripheral
requires at least100ms remaining on receipt. Its accepted local deadline and
clock generation stay frozen. Later fits cannot move an armed deadline. A
mapping change becomes visible only at another future reservation. Logical
phase cannot move backward within a phase epoch. A fresh plan after stale
coasting deliberately restarts a common phase at its future apply timestamp;
only that due edge clears the old phase floor on both roles. Normal phase uses64-bit timestamps and bounded
curve scaling, including uptime beyond the old32-bit elapsed range.

Clock exchange is on demand. Startup, reconnect, resume and an effective visual
change can begin a transaction. Changes include settings, advertising state,
and activity when the idle-off policy makes activity affect output. A no-op
endpoint or an activity transition with idle-off disabled does not begin one.
An acknowledged fit is reused only if it passes the unchanged future admission
bounds. A new PROBE retires readiness of the cached fit until an exact FIT_ACK;
it leaves the old presentation clock and visible output intact.

There is no periodic fit, periodic STATE, passive-expiry transmission or
automatic thirty-second recovery loop. Each transaction permits at most five
PROBEs total,500ms apart, and three immutable future-plan attempts. FIT success
does not refill that budget. Each plan permits at most three STATE copies200ms
apart while at least100ms lead remains. An exact ACCEPTED retires admission
copies; it does not confirm output. Coalesced edits do not refill an unresolved
transaction. Once it exhausts, unchanged intent stays silent until a new trigger.

The transaction's initial desired snapshot is its budget identity; each new
reservation still captures the latest desired values. At termination, a newer
latest value gets one new finite transaction unless already confirmed by own
successful output and exact peer APPLIED. A->B->A without successful completion
does not renew a budget. Genuine current own+peer completion ends a transaction
once; duplicate or obsolete ACKs cannot refill it. An already immutable next
plan remains charged to the next bounded transaction.

Missing APPLIED alone does not generate another plan. When a newer value waits
behind a successfully drawn reset-phase plan without peer confirmation, an
at-most200ms foreground wait allows the exact ACK to preserve continuity. Its
expiry permits a deliberate fresh-phase retry within the remaining budget.
There is no such timer without queued intent, and a consumed timer cannot spin.

## Wire and session identity

The explicit little-endian codec emits exactly56 bytes, including the relay's
source byte. The internal64-byte engine representation is unchanged; it is
never copied directly onto the version4 wire.

| Bytes | Fields |
| --- | --- |
| 0–3 | source, version, kind, status |
| 4–23 | central boot nonce, central link epoch, sequence, peer boot nonce, peer epoch (five32-bit fields) |
| 24–55 | kind-specific payload and zero reserved bytes |

Clock messages encode the applicable timestamps at24,32,40,48. PROBE uses only
the first, REPLY the first three, and FIT/FIT_ACK all four; unused bytes must be
zero. STATE encodes clock sequence at24, apply timestamp at28, phase epoch at36,
pattern/policy at44, brightness/policy at45 and speed at46. Pattern occupies the
low five bits of byte44, policy flags0–2 its high three bits. Brightness occupies
the low seven bits of byte45, flag3 its high bit. All18 patterns,0–100 brightness
values and16 flag combinations are represented without loss. STATE bytes48–55
must be zero. STATE_ACK uses those eight bytes for actual local completion;
ACCEPTED requires a zero completion timestamp. Every32-bit identity/sequence
and64-bit timestamp remains complete. Packing does not replace state with a hash.

Kinds are PROBE, REPLY, FIT, FIT_ACK, STATE, STATE_ACK and REQUEST. REQUEST has a
canonical zero32-byte payload. Clock payloads contain
the applicable64-bit timestamps. STATE contains clock sequence,64-bit apply
timestamp and phase epoch, pattern, brightness, speed and four policy/activity
flags. STATE_ACK echoes the complete STATE and includes actual local completion
time. Unknown versions/kinds/statuses, nonzero reserved bytes, invalid ranges,
wrong identities and stale or reordered state sequences are rejected. Exact
duplicates are idempotent; duplicate state can confirm APPLIED only after that
reservation's successful output completion. Completion originally requires a
usable clock; a later duplicate may report that historical success after expiry.
A delayed historical confirmation cannot restore fresh SYNCED after expiry;
it can only distinguish confirmed from unconfirmed coasting.

Both halves fence link boundaries. A probe only creates a candidate; it cannot
replace committed identity or an armed plan. A fresh, monotonically consumed
peer challenge must be echoed by FIT. This also supports unilateral central
PM/resume and sequence rotation without requiring a peer link event. Lower
observed central epochs and abandoned challenge echoes cannot rebind a session,
even when timestamp captures happen in the same integer millisecond. Sequence
exhaustion rotates the session; epoch exhaustion rotates boot identity.
The32-bit random boot nonce is probabilistic identity, not authentication.

The old `led`/`lea`, `ls2` version2 and periodic `ls3` version3 protocols are incompatible.
Both halves need matching version4 firmware. Distinct relay names and the
version/shape validator fail closed for mixed formats. The implementation
deliberately supports one split peripheral;
the central build rejects a configured peer count other than one. Stock ZMK
non-split support remains; this split relay path requires DYA.

On the configured production MTU65, subtracting three ATT bytes and three relay
header bytes leaves59 chunk data bytes. The three-character relay name consumes
three, leaving56 for the entire event including source. Each version4 kind fits
one relay chunk, compared with two for the old64-byte event. One is the minimum
nonzero relay chunk count; this is a framing result, not an airtime, radio-event,
power or throughput measurement.

The8000ms TTL,50ms admission/completion bound,1000ppm aging model and1000+2RTTms
lead remain unchanged. Removing background renewal deliberately permits the
halves to drift during a long unchanged presentation. Coasting reports that
loss of a current accuracy bound; it is not a longer admitted clock lifetime.
Neither the old periodic retries nor the new finite foreground retries guarantee
recovery before expiry under loss.

## Output, recovery and power

| State | Display | Meaning |
| --- | --- | --- |
| WAITING | Fixed waiting blink | No peer link |
| LINKED | Steady diagnostic | No usable completed reservation |
| RUNNING_UNCONFIRMED | Reserved normal pattern | Local reservation is running; complete output confirmation is pending |
| SYNCED | Reserved normal pattern | Required own/peer completion confirmed and clock quality is currently bounded |
| COASTING | Last successful normal pattern, free-running | Confirmed presentation continues after quality/session validity ends; current phase accuracy is unbounded |
| COASTING_UNCONFIRMED | Last successful normal pattern, free-running | Same unbounded phase, with complete output confirmation still absent |

All PWM drawing and reserved applies share a dedicated1536-byte split workqueue
at the existing low-priority thread priority. Relay sends remain on the separate
ZMK low-priority queue, where a potentially blocking core enqueue cannot occupy
the render queue. Every normal redraw consumes any due reservation before
sampling, so an older queued animation item cannot redraw obsolete state after
the reservation becomes due. Status indicator epochs remain local uptime.

Admission or actual output completion more than40ms after the frozen deadline,
expired reservation quality, and driver API failures reject that attempt and
consume the finite recovery budget. The last own-successful presentation is
kept. Failed new output restores its presentation metadata and queues a replay
of that older view, without rolling back newer desired values, current session,
fitted clock or counters. This is a driver-call recovery path, not proof of
physical restoration or rollback of the peer. Detection cannot undo an already late electrical edge.
ACCEPTED is admission only. APPLIED records driver API success, not a photometric
or electrical observation. ACK loss can leave RUNNING_UNCONFIRMED; a lossy channel
cannot guarantee atomic both-or-none delivery or simultaneous physical output.
There is no receipt-time phase fallback labeled synchronized.

PM first fences the engine's session under its short metadata lock, cancels any
running pattern draw, writes local zero, then drains apply and potentially
blocked TX work. The two existing early/late PM registrations remain idempotent.
An aborted central suspend starts a new clock transaction. An aborted peripheral
suspend can send up to five REQUEST hints500ms apart to the untouched central;
a completed FIT stops them. A hint only requests a challenge and current desired
future state; it never establishes a session. Identical hints are deduplicated;
same-boot epochs must increase. A genuinely newer accepted hint that arrives
during an older transaction retains an unsent challenge demand. If the old
budget ends before another PROBE can service it, that demand starts one fresh
finite transaction. An older pending plan's successful completion cannot consume
the unsent demand or certify the resumed peer session. A lifecycle sequence
fence keeps every pre-hint reservation in a coasting confirmation state and
preserves the newer presentation intent even if TX already emitted its PROBE
before the old PWM call completes. Only a later reservation sequence can
claim fresh synchronization, clear that intent or suppress its newer
latest-value promotion as already confirmed; link rotation resets the fence. The old wire snapshot, deadline and historical completion stay intact.
The hint leaves a locally in-flight reservation valid for its own completion
or failure recovery; its sequence fence alone excludes that old reservation
from fresh-session claims. A new central FIT still invalidates old-session
reservations. If their outstanding output call later returns, an uncompleted
obsolete attempt retires without constructing an ACK under the new session.
Its saved successful view is restored as invalid/coasting and queued for one
replay, or a diagnostic redraw is queued when no saved view exists. Current
pending reservations, queued new-session ACKs, desired values, fitted clock
and transaction budgets survive this local recovery. Link/PM teardown keeps
its no-active-view fence and cannot resurrect an earlier presentation.
A post-hint PROBE services the challenge demand within the remaining budget;
failure then stays silent, rather than granting five new attempts per hint.
A bounded four-entry retired-boot fence handles
rare nonce/epoch rotation, with no authentication or unlimited replay-history
guarantee. Previously queued relay traffic cannot be recalled, and local suspend
does not prove the peer turned off. The peer may keep its last successful output
coasting until another event-driven reservation. No BT, PWM, settings callback or synchronous cancel
wait runs while the metadata mutex is held.

## Evidence boundary

Canonical host, negative-control, actual Zephyr workqueue and firmware entries
are in [tests/README.md](tests/README.md). Host ordered traces use controlled
network timestamps. Native tests use the real kernel and controlled relay/PWM
APIs. Neither establishes physical BLE delivery, oscillator error, scheduling
guarantees, power usage, LED edges or end-to-end HID behavior. Production-board
acceptance for those claims remains a separate gate.
