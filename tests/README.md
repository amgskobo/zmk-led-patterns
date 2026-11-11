# Module-owned tests

Run from this checkout. Docker + Bash is the host entry point; no keyboard
checkout, shared test dispatcher or simulator repository is required.

```bash
bash tests/run-host-docker.sh
ZMK_TEST_MODE=full bash tests/run-host-docker.sh
```

On PowerShell, set `$env:ZMK_TEST_MODE="full"` for the second command
and invoke Git Bash explicitly. Remove the variable for the default quick run.

Quick executes **all behavioral fixtures and scenarios** once with
AddressSanitizer/UndefinedBehaviorSanitizer. Full repeats those same assertions
under the retained optimization, coverage and (where already present) 32-bit
variants. CI uses quick for push/PR and full for scheduled/manual runs.
Unknown mode values fail; there is no silent skipped success.

The assertion-based Python CLI checks require enabled assertions and reject
optimized Python before creating output or invoking tools. Default quick/full
also run `test_python_assertion_admission.py`: environment and `-O` admission
checks use a recording compiler and harmless zero-exit programs. These are
synthetic validator checks, not actual C mutation or kernel evidence. The same
fixture rejects nonassertion aborts in all four host mutation runners; a shared
result check requires SIGABRT and a qualified C assertion diagnostic.

## Retained behavioral contracts

| Fixture | Assertions |
| --- | --- |
| [runtime/standalone_harness.c](runtime/standalone_harness.c) | All curves, SOS long/short pulse order, state, clock, request fallback, activity and PM |
| [runtime/usb_only_harness.c](runtime/usb_only_harness.c) | USB-only advertising gate |
| [runtime/central_harness.c](runtime/central_harness.c) | Split desired/rendered state, status, all-pattern64-bit phase and PM |
| [runtime/peripheral_harness.c](runtime/peripheral_harness.c) | Same scheduled renderer assertions for the peripheral role |
| [runtime/brightness_behavior_harness.c](runtime/brightness_behavior_harness.c) | Brightness commands, bounds and readiness |
| [runtime/speed_behavior_harness.c](runtime/speed_behavior_harness.c) | Speed commands, bounds and readiness |
| [test_usb_pending.c](test_usb_pending.c) | Dispatch generations, independent fields, retries, port truth table and 100000 interleavings |
| [test_battery_adc_offset.c](test_battery_adc_offset.c) | Battery conversion, saturation and percentage mappings |
| [runtime/battery_harness.c](runtime/battery_harness.c) | Battery correction arithmetic |
| [runtime/settings_harness.c](runtime/settings_harness.c) | Same registry/value/save/notification scenarios through current and legacy APIs |
| [runtime/settings_presentation_harness.c](runtime/settings_presentation_harness.c) | Complete desired transport snapshot in the first immutable reservation; immediate desired, future visible edge, no-op/clamps and independent setters |
| [runtime/settings_ble_only_harness.c](runtime/settings_ble_only_harness.c) | BLE-only settings and port gates |

Each fixture keeps its existing normal, boundary/invalid, independent-state,
reset/cancellation and failure/recovery scenarios where applicable. Checks
assert ordered outputs and explicit time inputs. Coverage is a full-mode gate,
not a reason to replace assertions or a hardware completeness claim.

The standalone runtime fixture calls the real public `led_pattern_request` with
invalid field selectors and requires no settings, state, event or driver side
effects. Both split runtime fixtures require no adapter forwarding or state
notification for those invalid public requests. The native direct-adapter
selector check below is a separate boundary.

## Separate integration and physical contracts

Keep the existing owner `run-integration-docker.sh` commands and accepted
upstream/DYA variants for changed APIs, DTS/Kconfig/registration or actual
kernel boundaries. Native fixtures and negative guards are retained. Do not
run all firmware variants for a pure arithmetic edit. Host extracted functions
and controlled work queues do not prove real scheduling or transport delivery.

## Retired legacy simulation

The active `tests/bsim` directory was removed on 2026-10-05. Before removal,
every source and ordered snapshot was SHA256-verified in an external archive.
Its retirement manifest retains the raw traces and assertion mapping. Archive
locations belong to the consuming keyboard integration repository and are not
a prerequisite for these module-owned checks.
No failed snapshot was accepted as passing.

| Retired case | Behavioral replacement | Transport boundary |
| --- | --- | --- |
| battery-sag | The owner fixtures above retain state/arithmetic/error assertions | Ordered physical split/HID/LED/battery delivery and timing: pending actual-device evidence |
| split-mirror | The owner fixtures above retain state/arithmetic/error assertions | Ordered physical split/HID/LED/battery delivery and timing: pending actual-device evidence |

For the affected case, exercise the same contact/layer/settings/power or
congestion/reconnect transition on the production board. Capture each report
value/order/timestamp and verify balanced releases and specified deadlines.
Do not mark the archived exact radio trace as covered by a host stub.

## Current test ownership and compatibility

Run the relevant public interface/build checks on both unchanged stock ZMK
and DYA when this owner supports both. See the selected core in the manifest.

Canonical entries (run from the repository root):

```bash
bash tests/run-host-docker.sh
bash tests/run-integration-docker.sh
```

[maintenance.json](maintenance.json) records supported cores, current commands
and explicit gaps. It is a reference map, not a passing test or a claim that
registration/coverage totals establish completeness. Preserve ordered outputs,
raw timestamps and the first failure. Read actual dependencies before execution.

## Explicit offline integration

The canonical `tests/run-integration-docker.sh upstream|dya` wrapper accepts host
paths for the variables below. Its direct SDK entry,
`tests/integration/run.sh upstream|dya`, accepts container-visible paths for the
same variables. The direct entry accepts
`ZMK_TEST_OFFLINE_WORKSPACE=/absolute/resolved/workspace` and
`ZMK_TEST_OUTPUT=/absolute/new/output`. This path performs no initialization or
network update, requires the pinned unchanged stock/DYA core and Zephyr, records
their identities, and rejects an existing evidence path before building. Resolve
and read all actual dependency instructions before selecting it. All existing
firmware/native/configuration/registration assertions remain enabled. The
ordinary wrapper retains its dependency-setup behavior. The Docker wrapper maps
explicit host paths as shown below; direct SDK callers use paths visible inside
their container.

The default DYA fixture pins its original custom-settings dependency at
`c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f` and requires a clean checkout. An
additional `ZMK_TEST_CUSTOM_SETTINGS_MODULE=/absolute/resolved/module` selects
an explicitly audited current registry override; record that checkout's source
identity separately. It does not replace the pinned dependency compatibility
check. `ZMK_TEST_LED_ROLE=central|peripheral` additionally compiles the real
DYA split relay paths. Central retains the settings bridge; peripheral follows
the core's peripheral role without Studio. Stock split relay is unsupported
and reported NOT APPLICABLE, never a feature-disabled compatibility pass.

## Docker arguments, retained results and CI

The integration wrapper validates core/role before invoking Docker and forwards
`ZMK_TEST_LED_ROLE=standalone|central|peripheral`. Stock split is rejected as
unsupported. The default DYA fixture still checks its original clean c6a7
registry; a current override is an additional compatibility case. For example:

```bash
bash tests/run-integration-docker.sh upstream
bash tests/run-integration-docker.sh dya
ZMK_TEST_LED_ROLE=central bash tests/run-integration-docker.sh dya
ZMK_TEST_LED_ROLE=peripheral bash tests/run-integration-docker.sh dya
ZMK_TEST_CUSTOM_SETTINGS_MODULE=/absolute/current-settings bash tests/run-integration-docker.sh dya
ZMK_TEST_OUTPUT=/absolute/new/online-result bash tests/run-integration-docker.sh dya
ZMK_TEST_OFFLINE_WORKSPACE=/absolute/resolved/workspace ZMK_TEST_OUTPUT=/absolute/new/offline-result bash tests/run-integration-docker.sh dya
python3 -m unittest discover -v -s tests -p 'test_integration_*.py'
```

Wrapper paths name directories on the Docker host, including PowerShell callers
using an absolute forward-slash path. The source, explicit registry override and
offline workspace are mounted read-only at `/src`, `/custom-settings` and
`/workspace`; environment values are rewritten to those container paths. An
explicit output parent is mounted read/write at `/results`. The requested result
directory must not exist, including as a symlink; the owner runner creates it
atomically. Offline selection additionally requires a resolved `.west` workspace
and runs Docker with `--network none`, with no west initialization/update. Online
selection downloads its declared dependencies; it has no offline/no-network claim.

Both online and offline entries retain an explicit `ZMK_TEST_OUTPUT`, including
partial results on failure. After online dependency setup, `resolved-west.yml`
records the actual active-project SHAs before compilation using
`west manifest --freeze --active-only`, matching the projects selected by normal
`west update`. Inactive optional projects are not forced into the tested graph. A failed freeze
propagates its real exit and never publishes a successful identity file. Online
dependency workspaces remain temporary and are cleaned separately. Without explicit output, online results are temporary. Docker
`--rm` removes only the test container; it never deletes host evidence or caches.
`test_integration_docker.py` inspects actual wrapper argv via a recording Docker
executable. `test_integration_lifecycle.py` uses failing west tools to check retained
outputs, real exits and temporary-workspace cleanup. Both run in quick/full. They
do not establish firmware success; the real firmware validator remains required.
`integration/test_validation.py` checks the actual validator's source admission
from the resolved owner checkout and rejection of an unrelated source path.
Its synthetic artifacts stop before ELF inspection. Real firmware validation
uses canonical owner/registry source paths, including a direct SDK checkout
outside `/src` and a registry selected through a workspace symlink.

CI dispatches these same entries: host quick on push/PR and full on manual/schedule;
four real firmware jobs (stock standalone, DYA standalone/central/peripheral); and
both native roles against exact Zephyr revision
`10ba6d0cb38bc3d258775d27982f707599320085`. The native job mounts only the owner,
that kernel checkout and a fresh result parent, and runs without network. It does
not initialize or import an ambient west graph. The SDK image is pinned in
[the workflow](../.github/workflows/test.yml). Separate documented native controls
remain additional checks; the CI native job runs the positive role matrix.

Each CI job uploads its own raw log; firmware/native jobs also retain their result
artifacts, including `.config`, under a unique run/attempt name. Host temporary
binaries are not retained by the default wrapper. CI artifact retention is finite
and a hosted CI run is separate evidence from a local run. Moving online core
branches are compatibility checks; fixed offline pins identify reproducible
maintenance runs. Neither is physical USB/BLE/PWM/power acceptance.

## Scheduled clock, protocol and kernel checks

The default quick/full entries also execute `test_led_sync_clock.c` and
`test_led_sync_protocol.c`. Clock math covers checked signed conversion,
asymmetric/queued samples, integer timestamp quantization, future-time quality,
expiry and overflow. Protocol tests drive two instances of the production
engine with raw ordered timestamps. The protocol fixture also bounds each
engine to 792 bytes on the supported firmware/native ABIs: the active plan
owns its frozen clock, while a candidate fit and saved rollback plan remain
independent. The original duplicate-clock layout fails this storage assertion.
Named cases cover future apply on both
roles, immutable reservations/latest-next coalescing, malformed shape/session
and sequence replay, same-ms candidate challenges, unilateral PM/session
rotation, driver completion/late/quality failures, exact ACK identity,
RUNNING_UNCONFIRMED,64-bit uptime, and a30s continuous change stream whose required foreground clock exchange
cannot starve.

Full preserves every existing100% helper/runtime line+branch gate and adds the
clock helper gate. New protocol coverage is reported separately; it is not a
claim of exhaustive protocol/hardware coverage. Eight identified exact-expression
negative controls compile successfully and run the same binary three times.
Compile failure is not a killed control. Their canonical standalone entry is:

```bash
python3 tests/protocol_negative.py /absolute/fresh/output
bash tests/native/run_scheduled.sh /absolute/resolved/workspace /absolute/fresh/output
python3 tests/native/controls.py run /absolute/resolved/workspace /absolute/fresh/native-controls
python3 tests/native/controls.py validate /absolute/retained/native-controls /absolute/fresh/control-report
python3 -m unittest discover -v -s tests/native -p test_controls.py
```

The native entry builds unchanged emitted production adapter/controller
definitions against real Zephyr mutexes, atomics, work queues and synchronous
cancellation. Central and peripheral binaries each run three times with every
named completion required. Relay, Bluetooth and PWM APIs remain controlled.
The checks include a blocked TX across a reserved draw, due commit before an
older queued animation, frozen mapping/deadline, PM fence/zero before blocked
TX drain, unilateral resume with an untouched counterpart engine, independent
field requests, rejected RX preserving desired state, and own/peer completion
stage order. Current native checks also reject invalid adapter field selectors
by calling `led_scheduled_request` directly, without side effects, retire actual retry timeouts on disconnect
while preserving required redraw, and accept new work while an older relay call
is blocked. A parked K_FOREVER delayable-work descriptor can remain marked
DELAYED while its actual timeout remaining is zero; that is not an active wakeup.
Rescheduling alone does not remove an already queued/running invocation.
They do not prove physical BLE scheduling or electrical output.

`native/run.sh` is a compatibility alias for `native/run_scheduled.sh`; it does
not supply another suite. Default quick/full invoke `--test-admission` through
this alias with a combined ten-second timeout. Existing directories, dangling
symlinks and directory symlinks reject before dependency/build access; sentinel
hashes and the absent target are preserved. No kernel build is performed.

Optional identified controls are `--share-render-tx-queue`, `--old-draw`,
`--old-request`, `--early-sync-stage`, `--old-rx-disposition` and
`--accept-noncanonical-wire`, `--coast-wakeup`, `--retry-exhausted`,
`--no-coast-rebase`, `--idle-off-activity`, `--no-render-rollback`,
`--skip-replay` and `--cancel-running-tx`. These
must fail their unchanged assertions and are never accepted production builds.

`native/controls.py run` is the canonical selection/validation entry for all
thirteen modes and eighteen role variants. It calls the existing native runner
and reuses each compiled binary for three executions. Acceptance requires the
intended scenario itself to fail an assertion in every repeat; a failure in an
unrelated test, a timeout or a compiler/setup failure cannot kill a control.
The owner checks exact role/mode/compiler flags, all required scenario outcomes,
all fourteen current source inputs, generated source and binary SHA256 values.
It retains the ordered raw logs and writes `results.json`.

The `validate` action reads an existing complete control result tree and creates
a fresh report without compiling or executing a binary. It verifies the same
source/binary/role/target evidence; it is retained-evidence validation, never an
additional set of kernel executions. Earlier external aggregation scripts are
historical reporting only and supply no current assertions or mode selection.

`native/test_controls.py` runs parser/dispatcher scenarios with
subtests in both default quick and full. Its fake ELF/log records and recording
runner are explicitly synthetic data, never kernel or hardware evidence. Cases
reject unrelated failures, target blocks without assertions, timeouts, missing
repeats/binaries/hashes, source drift/subsets, wrong roles/modes/compiler flags
and reused/symlink outputs. Dispatcher tests preserve the exact commands and real
outer exit codes and report setup/launch errors without inventing a runner exit.
The actual native positive matrix remains 99 completions, and
the control matrix remains 54 expected assertion failures.

The prior receipt-phase central/peripheral and native sources, hashes and raw
assertions were retained in an external historical archive before replacement.
That archive is optional provenance; it supplies no assertions or prerequisites
for this checkout. The current replacement map is below:

| Previous assertion | Current owner fixture |
| --- | --- |
| Old ACK preserves newer pending state; clean retry sends nothing | `test_scheduled_and_desired` and `test_bounded_traffic_and_loss`: immutable in-flight/latest-next, exact identity, admission retires copies |
| Retry cannot replace a short state deadline | Native blocked-TX and due-before-queued-animation cases; frozen clock/deadline protocol case |
| Five handshake sends; stale/pre-send/disconnected ACK rejects | Finite clock attempts/exhaustion and both-session/sequence/complete-state ACK checks |
| Suspend blocks scheduling; independent resume requeues state | Actual native fence/zero/drain and unilateral-resume cases, plus two-engine session rotation |
| Peripheral accepts/clamps old malformed wire fields | The current version strictly rejects malformed wire state; local setters still clamp, checked by controller/native interfaces |
| Either activity source could wake a peripheral immediately | Intentional contract change: central normal activity is reserved on both; local activity remains diagnostic-only |
| WAITING/LINKED/SYNCED displays and curve output | Both runtime role fixtures retain fixed local status, normal curves/brightness/speed/idle/advertising, zero-output and PM checks; add RUNNING_UNCONFIRMED and both COASTING stages |

Settings callback overlap has its own actual pthread interleaving fixture,
`runtime/settings_interleaving_harness.c`. It pauses an older callback after a
registry read while a newer writer commits. USB and BLE must finish with latest
registry and desired values. Its fixture-only no-lock control reproduces the
old obsolete callback finishing last. The owner emitter supports
`python3 tests/runtime/run.py --settings-interleaving NEW_SOURCE_OUTPUT` for
that focused source, without changing the default all-scenario run.

`runtime/settings_presentation_harness.c` joins the actual settings callback and
adapter setters/publish with the production engine. It checks that a transport
switch reserves the complete desired snapshot on its first attempt, while the
old visible state remains until the future edge. Registry, kernel and controller
forwarding calls are controlled; this is not real-kernel or radio evidence. The
focused emitter is `python3 tests/runtime/run.py --settings-presentation NEW_SOURCE_OUTPUT`.

## Version4 wire, on-demand synchronization and ordered traffic

Quick/full run all original clock/protocol/codec/traffic scenarios and
`test_led_sync_on_demand.c`. The new two-engine fixture covers hour-long idle
silence, fresh-fit reuse, long backwards refit with a new phase only at a future
edge, finite failed exchanges/plans, prior-view rollback, bounded queued-reset
confirmation, unilateral peripheral resume REQUEST, latest-once budget promotion,
duplicate/obsolete completion accounting, immediate invalid-session COASTING,
and unacknowledged candidate fits never authorizing a promoted reservation.
Newer peripheral resume hints retain unserviced demand across old transaction
exhaustion or completion. Fresh transaction begin or an actual post-hint PROBE
consumes it; a hint does not guarantee five extra attempts. Duplicate hints and
a failed serviced burst cannot rearm passive traffic.
An accepted newer resume hint also fences fresh completion by the sequence
current at admission. An old immutable plan may finish and retain historical
success accounting, but cannot become freshly SYNCED or clear newer intent,
even after a post-hint PROBE consumes the hint latch. The fence survives
prior-view rollback and resets on link rotation without increment overflow.
Historical confirmation before a resume cannot suppress a newer latest-value
transaction after the hint has been serviced. REQUEST during an installed but
uncompleted draw preserves its success/failure handling and future recovery.
If a new central-session FIT invalidates an outstanding old draw, its completion
retires only that obsolete output: restore the last own-successful view as
invalid/COASTING, or redraw status when none exists. New pending reservations,
fit/session, desired edits, retry accounting and an already queued new-session
ACCEPTED ACK remain unchanged; no old completion ACK uses the new identity.
Duplicate completion of a restored coasting view does not replay it again, and
link/PM teardown cannot resurrect an old view.
It uses the production codec and engines. `run-on-demand.sh` builds optimized
and ASan/UBSan binaries and repeats each identical binary/input three times.

The codec retains the independent complete56-byte FIT and APPLIED golden vectors,
distinct wide timestamps,18x101x16 state/flag combinations, exact widths/ranges,
all kinds/statuses and malformed lengths/tails. Version4/ls4 adds zero-body
REQUEST; versions2,3 and5 are explicitly rejected. The full event including
source remains56 bytes. At configured MTU65, three name bytes plus56 event bytes
fit the59-byte relay chunk capacity. Paired version4 firmware is required.

Canonical additional commands, each with a fresh output path:

```bash
bash tests/run-on-demand.sh /absolute/fresh/output
python3 tests/on_demand_negative.py /absolute/fresh/output
bash tests/run-traffic.sh /absolute/fresh/output
python3 tests/traffic_fixture.py /absolute/fresh/pinned-output
python3 tests/traffic_compare.py BASELINE_OUTPUT CANDIDATE_OUTPUT FRESH_COMPARISON_OUTPUT
python3 tests/test_traffic_compare.py BASELINE_OUTPUT CANDIDATE_OUTPUT FRESH_SENSITIVITY_OUTPUT
python3 tests/traffic_negative.py /absolute/fresh/output
python3 tests/wire_negative.py /absolute/fresh/output
```

`run-wire.sh` is a compatibility alias for `wire_negative.py`, not a second
codec sensitivity suite. Both entries run the unchanged codec fixture and six
identified production-code mutations.

The baseline/candidate fixture uses
the same10ms queues,10000ms offset and60s idle/30s continuous edits/45s loss plus
reconnect inputs. Three identical-binary repeats must match within each policy.
Every apply is checked against its immutable recorded full state and phase,
future edge and40ms bound; ordered reservations also match the independent input
schedule. A peer apply matches the central plan. The comparator checks ordered
intent/edges/state/recovery as well as traffic, and its corruption controls
cannot be bypassed by three reproducibly wrong traces or reduced totals.

The controlled baseline/candidate counts are132->7,103->87 and159->152 packets
(also relay chunks, one per event). Per-kind and raw ordered rows are retained.
The unchanged60s case sends no LED packet after the initial seven-message
transaction. These are deterministic software traffic results, not electrical
power, airtime, radio-event or physical phase measurements.

Quick/full comparator inputs reject eight deliberate identical trace corruptions
even when all three parser-input copies match. Full retains eight protocol and
six codec production-expression controls and
adds identified on-demand and ordered-traffic controls. Each unchanged control
passes three runs; each exact mutant must compile and fail unchanged assertions
three runs. Compile failure or a missing selector is a setup failure. The old
early-SYNCED stage-assignment operator became inactive after centralized stage
classification; its replacement removes the actual own-completion predicate.
The old periodic-starvation operator now removes demand-fit scheduling while
retaining the continuous-change admission assertions. Native controls additionally
exercise real queue wakeups, finite exhaustion, future rebasing, effective
activity, rollback metadata and successful prior-view replay after PWM failure.

Default quick/full also run all eight comparator corruption cases. Their historical
input is the pinned27,634-byte output fixture in `tests/fixtures/`, with exact source
and trace provenance; it contains no old algorithm. Quick executes the current
engine once under sanitizers, then copies its output into parser-input slots.
Those copies are explicitly not claimed as binary reruns. Full reuses its
already compiled optimized traffic binary for three genuine executions.
The standalone historical/candidate CLI remains available for three real
baseline and candidate runs and preserves raw comparison evidence separately.
The comparator checks all three input slots for exact byte identity, then parses
the reference pair once. Each positive or corrupted parser input is evaluated
once; repeating the same deterministic parse is not a second engine execution.

### Optional historical rebuild

The default historical parser input is the pinned fixture above. Quick/full do
not require an external archive or an older implementation. For historical
investigation only, the following commands can rebuild the archived baseline:

```bash
python3 tests/traffic_baseline.py ARCHIVED_SELECTED_ASSERTIONS_JSON /absolute/fresh/headers
bash tests/run-traffic.sh /absolute/fresh/baseline baseline56 /absolute/fresh/headers
```

`traffic_baseline.py` admits only the exact SHA-pinned original periodic3
protocol/codec and unchanged clock helper. It does not rewrite a dependency or
algorithm. This optional reconstruction is separate from current compatibility
or release acceptance.

The native suite preserves all nine prior cases per role and adds quiet steady
coasting with worker counts, animation continuing without radio, a300s monotonic
relative-clock coast and fresh future reset, failed transaction silence/rearm,
and central idle-off/activity normalization. A failed new sequence cannot claim
completion; restoring an older successful sequence can legitimately restore its
confirmed stage. Ordered driver-call assertions require failed81 followed by a
successful23 replay, without inferring physical restoration. Each central17/peripheral16 binary
runs three times (99 completions). Thirteen native control modes select eighteen
role variants, each required to fail its unchanged assertions three times
(54 expected failures); compile/setup failures do not kill a control.
Quantization is derived from the actual
native100Hz tick (20ms allowance), never a fixture-only2ms seed.

### Intentional assertion replacement

Before these timing semantics changed, affected source/assertions and hashes
were retained in an external historical archive. The optional archive is not
needed for quick/full: the SHA-pinned parser fixture and its provenance live in
[fixtures/led-sync-periodic-v3.json](fixtures/led-sync-periodic-v3.json).
Unique admission/shape/session/exactACK/64-bit/own-completion assertions remain.

| Prior assertion | Intentional current contract and replacement |
| --- | --- |
| Fit/STATE refresh every3s,30s recovery backoff forever | No passive traffic; on-demand hour silence, finite5PROBE/3plan exhaustion, one latest promotion and no-op/A-B-A assertions |
| Active expiry drops normal output | Both COASTING stages preserve last successful phase/output; freshness timers are consumed without worker spin |
| Global old-phase floor after a refit | Monotonic within one phase; explicit stale->fresh reset at a future common edge, including backwards-fit and missed/reset recovery |
| Failure leaves a newly failed view/diagnostic | Prior own-successful presentation metadata is restored and replayed; newer desired/session/budget stays intact |
| All periodic/candidate trace rows equal | Same inputs retained; intentional quiet/foreground traces have per-reservation state/time/order/recovery assertions, repeat equality and corrupt-trace controls |
| Kind<=6 and version3 literals | REQUEST7 canonical zero body, full golden bytes updated only version4, explicit predecessor3 rejection |
| Unilateral resume relies on periodic central refresh | Finite REQUEST hint/challenge, no artificial counterpart reset; native untouched-peer case retained |

Physical BLE/electrical/power acceptance and exhaustive adversarial traffic remain
gaps. Free-running phase drift during coasting is intentional. Five attempts
under loss do not guarantee completion before expiry. A bounded retired-boot
history and32-bit random boot identity are not authentication.

### Test consolidation

Removed sources and raw failure logs were archived externally before
consolidation. The current suite needs no archive. The replacement map is:

| Removed repetition | Retained assertion or boundary |
| --- | --- |
| Python native-output admission fixture | `native/run_scheduled.sh --test-admission`: existing directory, dangling and directory symlinks reject before dependency/build access; sentinel/hash unchanged; default dispatch retains a ten-second bound |
| Three comparator processes for each identical parser input | One process for the positive and each of eight corrupted inputs, preserving every corruption and its expected acceptance/rejection |
| Parsing three byte-identical trace pairs in each comparator process | All three input pairs still require exact byte identity and individual hashes; the reference pair retains every ordered value, phase, deadline and recovery assertion |
| Fixture-only dropped flag and cleared malformed tail | `wire_negative.py` mutates the actual encoder flag expression and decoder tail guard; the same full-width roundtrip and malformed-tail rejection assertions remain, with all six production controls |

Full still executes its optimized traffic engine binary three times. Native
kernel roles, compiler/ABI variants and identified production mutations remain
separate checks; none is replaced by the deterministic parser consolidation.

## USB coalescer contract replacement

Only notifications are deferred now; key writes commit registry values
immediately. The old value buffer and overlay were removed because they could
overwrite a later client edit. Their complete pre-change helper and assertions
were retained in an optional external historical archive before removal. No
private checkout or task directory is required by the current suite.
The replacement map preserves the responsibilities that remain applicable:

| Previous assertion | Current fixture / contract |
| --- | --- |
| Latest buffered value after 1000 updates | `settings_harness.c::test_usb_store`: latest immediate registry/LED value and persisted value |
| Independent pattern/brightness/speed values and overlays | `test_registry_fields_are_independent` and `test_apply`: immediate values, unchanged independent fields, and no-op application |
| New value during an older write and failed finish | `test_usb_pending.c::test_write_race_and_retry`, plus settings notification hooks: newer dispatch generations remain pending |
| 100000 interleavings | The helper retains the same field/action/success sequence, including former value RNG draws; values are checked at their registry owner |
| Failed deferred MEMORY write | MEMORY writes now return synchronously: settings fixture asserts no new dirty/notification state on failure and subsequent successful recovery; standalone `test_requests` asserts the real caller's direct fallback |
| Port-open truth table | `test_usb_pending.c::test_port_state` and settings ready/DTR/error cases |
| Delayed save of a value read earlier | `test_newer_client_during_save`: registry atomic save preserves the newer client value |

The current and legacy API fixtures run identical scenarios. Current quiet
events scope suppression to that call. The original c6a7 fallback retains its
dependency's global-suppression concurrency gap. A controlled negative return
from event dispatch tests bookkeeping only: the actual Studio listener reports
its queue/transport errors separately, so a successful dispatch cannot prove
queue admission, congestion recovery or delivery.

## Exact firmware registration checks

The integration runner dispatches `tests/integration/validate.sh` for all build
assertions. It requires six exact **defined** ELF symbols: device, init and PM
objects for both early and late LED registrations. Printable bytes before an
ELF name cannot supply or remove registration evidence. The four unittest groups
in `test_registration.py` run in quick and full; each missing object, suffix-only
lookalike, undefined reference and strings-only substitute is rejected.

An already retained build can be validated without rebuilding or overwriting it:

```bash
bash tests/integration/validate.sh BUILD_ROOT upstream|dya ROLE FRESH_VALIDATION_OUTPUT [SETTINGS_MODULE]
```

The fresh validation output retains input artifact hashes, symbols and strings.
All existing configuration, battery selection, single LED owner, behavior,
schema and selected settings-source assertions remain enabled.
