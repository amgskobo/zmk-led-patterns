#!/usr/bin/env python3
"""Validate ordered fixed-input traces before comparing periodic/on-demand cost."""
import argparse
import hashlib
import json
from pathlib import Path

DURATIONS = (60000, 30000, 45000)


def require(condition, context):
    if not condition:
        raise ValueError(context)


def intent(scenario, time):
    if scenario == 0:
        return (1, 80, 100)
    return (1, (time // 10) % 101, 10 + (time // 10) % 391)


def parse(raw, label, legacy=False):
    lines = raw.decode().splitlines()
    cases = []
    current = None
    finished = False
    for number, line in enumerate(lines, 1):
        context = f"{label}:{number}: {line}"
        fields = line.split()
        require(bool(fields), context + ": empty row")
        tag = fields[0]
        if tag == "scenario":
            require(len(fields) == 4 and fields[2] == "duration", context)
            index, duration = int(fields[1]), int(fields[3])
            require(index == len(cases) and index < 3 and duration == DURATIONS[index], context)
            if current is not None:
                require(current["summary"] is not None, context + ": preceding case incomplete")
            current = {"index": index, "duration": duration, "header": line, "rows": [],
                       "tx": [], "rx": [], "plans": {}, "applies": [[], []], "final": None,
                       "summary": None, "last_time": 0, "linked": True, "generation": 0,
                       "segment_ends": {}, "wire_due": False, "links": []}
            cases.append(current)
            continue
        if line == "led sync traffic: PASS":
            require(current is not None and current["summary"] is not None and number == len(lines), context)
            finished = True
            continue
        require(current is not None and current["summary"] is None, context + ": outside open case")
        if tag == "wire":
            require(current["wire_due"] and fields == ["wire", "56", "1"], context)
            current["wire_due"] = False
            continue
        require(not current["wire_due"], context + ": missing wire row after TX")
        if tag == "final":
            values = tuple(map(int, fields[1:]))
            require(len(values) == 6 and values[0] == current["index"] and current["final"] is None, context)
            current["final"] = values[1:]
            continue
        if tag == "summary":
            require(len(fields) == 18 and fields[2] == "packets" and fields[4] == "lost" and
                    fields[6] == "applies" and fields[9] == "kinds" and fields[-1] == "PASS", context)
            require(int(fields[1]) == current["index"] and current["final"] is not None, context)
            current["summary"] = {"packets": int(fields[3]), "lost": int(fields[5]),
                                  "applies": [int(fields[7]), int(fields[8])],
                                  "kinds": list(map(int, fields[10:17]))}
            continue
        require(current["final"] is None, context + ": temporal row after final state")
        values = list(map(int, fields[1:]))
        require(tag in ("link", "reserved", "tx", "rx", "apply"), context + ": unknown row")
        time = values[0]
        require(current["last_time"] <= time <= current["duration"] and time % 10 == 0,
                context + ": raw time/order violated")
        current["last_time"] = time
        current["rows"].append(line)
        if tag == "link":
            require(len(values) == 2 and current["index"] == 2 and
                    (time, values[1]) in ((18000, 0), (20000, 1)), context)
            require(current["linked"] != bool(values[1]), context + ": duplicate link edge")
            current["links"].append(tuple(values))
            if not values[1]:
                current["segment_ends"][current["generation"]] = time
            else:
                current["generation"] += 1
            current["linked"] = bool(values[1])
        elif tag == "reserved":
            require(len(values) == 9 and current["linked"], context)
            _, boot, epoch, sequence, edge, phase, pattern, brightness, speed = values
            state = (pattern, brightness, speed)
            require(boot > 0 and epoch > 0 and sequence > 0 and 0 <= phase <= edge, context)
            # Fixed10ms one-way queues produce RTT20ms:1000+2*20ms lead.
            require(edge - time == 1040, context + ": future reservation lead changed")
            require(state in (intent(current["index"], time), intent(current["index"], max(0, time - 10))),
                    context + ": reservation did not capture latest deterministic intent")
            key = (sequence, edge)
            require(key not in current["plans"], context + ": duplicate reservation identity")
            prior = [p for p in current["plans"].values() if p["generation"] == current["generation"]]
            require(not prior or (sequence > prior[-1]["sequence"] and (boot, epoch) == prior[-1]["session"]),
                    context + ": session/sequence order changed")
            current["plans"][key] = {"at": time, "sequence": sequence, "edge": edge,
                                      "phase": phase, "state": state, "session": (boot, epoch),
                                      "generation": current["generation"], "sends": 0, "draws": {}, "peer_rejected": False}
        elif tag == "tx":
            require(len(values) == 6 and current["linked"], context)
            _, role, kind, status, sequence, dropped = values
            require(role in (0, 1) and 1 <= kind <= 7 and 0 <= status <= 5 and sequence > 0 and dropped in (0, 1), context)
            expected_drop = current["index"] == 2 and 10000 <= time < 18000 and role == 1 and kind in (4, 6)
            require(bool(dropped) == expected_drop, context + ": loss injection changed")
            if kind == 5:
                plans = [p for p in current["plans"].values() if p["sequence"] == sequence and
                         p["generation"] == current["generation"] and p["at"] <= time]
                require(role == 0 and status == 0 and len(plans) == 1, context + ": STATE lacks reservation")
                plan = plans[0]
                plan["sends"] += 1
                require(plan["sends"] <= 3 and time <= plan["edge"] - 100, context + ": STATE retry/deadline violated")
            if kind == 6 and role == 1 and status >= 3:
                plans = [p for p in current["plans"].values() if p["sequence"] == sequence and
                         p["generation"] == current["generation"]]
                require(len(plans) == 1, context + ": rejection lacks reservation")
                plans[0]["peer_rejected"] = True
            current["tx"].append(values)
            current["wire_due"] = True
        elif tag == "rx":
            require(len(values) == 6 and values[1] in (0, 1) and values[5] in (0, 1), context)
            current["rx"].append(values)
        else:
            require(len(values) == 9 and current["linked"], context)
            _, role, sequence, edge, phase, pattern, brightness, speed, stage = values
            require(role in (0, 1) and (sequence, edge) in current["plans"], context + ": draw lacks reservation")
            plan = current["plans"][(sequence, edge)]
            require(plan["generation"] == current["generation"] and role not in plan["draws"], context)
            require(0 <= time - edge <= 40, context + ": draw outside common future edge/40ms bound")
            require((pattern, brightness, speed) == plan["state"] and phase == plan["phase"],
                    context + ": draw changed frozen reservation state/phase")
            require(stage == (2 if role == 0 else 3), context + ": completion stage violated")
            previous = [p for p in current["applies"][role] if p["generation"] == current["generation"]]
            require(not previous or sequence > previous[-1]["sequence"], context + ": draw sequence reordered")
            if role == 1:
                require(0 in plan["draws"] and plan["draws"][0] == time,
                        context + ": peer did not share central's exact controlled edge")
            plan["draws"][role] = time
            current["applies"][role].append(plan)
    require(finished and len(cases) == 3, label + ": incomplete output")
    for case in cases:
        context = f"{label}: scenario{case['index']}"
        require(not case["wire_due"] and case["summary"] is not None, context)
        delivered = set()
        for time, to_peer, kind, status, sequence, _accepted in case["rx"]:
            matches = [i for i, row in enumerate(case["tx"]) if row ==
                       [time - 10, 1 - to_peer, kind, status, sequence, 0]]
            require(len(matches) == 1 and matches[0] not in delivered,
                    context + ": RX does not match one ordered10ms delivery")
            delivered.add(matches[0])
        require(all(dropped or time + 10 > case["duration"] or i in delivered
                    for i, (time, _role, _kind, _status, _sequence, dropped) in enumerate(case["tx"])),
                context + ": delivery disappeared")
        for plan in case["plans"].values():
            end = case["segment_ends"].get(plan["generation"], case["duration"] + 1)
            if plan["edge"] < end and plan["edge"] <= case["duration"]:
                # The retained periodic baseline can emit an unconfirmed plan
                # after lost FIT_ACK; preserve its explicit peer rejection.
                # Current candidate must always complete both roles here.
                legacy_reject = (legacy and case["index"] == 2 and
                                 10000 <= plan["edge"] < 18000 and
                                 plan["peer_rejected"] and set(plan["draws"]) == {0})
                require(set(plan["draws"]) == {0, 1} or legacy_reject,
                        context + ": reserved common draw disappeared")
        require(all(case["applies"]), context + ": no successful presentation")
        last_c, last_p = case["applies"][0][-1], case["applies"][1][-1]
        require(last_c is last_p and last_c["generation"] == case["generation"], context + ": final roles diverged")
        pattern, brightness, speed = last_c["state"]
        require(case["final"] == (last_c["sequence"], pattern, brightness, speed, last_c["phase"]),
                context + ": final state does not match last common presentation")
        counts = [sum(row[2] == kind for row in case["tx"]) for kind in range(1, 8)]
        summary = case["summary"]
        require(summary == {"packets": len(case["tx"]), "lost": sum(row[5] for row in case["tx"]),
                            "applies": list(map(len, case["applies"])), "kinds": counts}, context + ": summary hid row mismatch")
        if case["index"] == 2:
            require(case["links"] == [(18000, 0), (20000, 1)] and summary["lost"] > 0, context)
            require(all(any(plan["generation"] == 1 and plan["draws"][role] > 26000
                            for plan in case["applies"][role]) for role in (0, 1)),
                    context + ": post-reconnect common recovery missing")
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit("fresh comparison output required")
    args.output.mkdir(parents=True)
    reports = []
    reference = None
    rows = None
    for repeat in range(1, 4):
        raw = tuple((folder / f"repeat-{repeat}.log").read_bytes() for folder in (args.baseline, args.candidate))
        if reference is None:
            reference = raw
        require(raw == reference, f"repeat{repeat}: identical-binary/input trace changed")
        if rows is None:
            # Byte equality above binds every input slot to this validated pair.
            # Repeating the deterministic parse adds no new behavioral input.
            old, new = (parse(data, f"{'baseline' if role == 0 else 'candidate'} repeat1", legacy=role == 0) for role, data in enumerate(raw))
            rows = []
            for index, (a, b) in enumerate(zip(old, new)):
                require(len(a["tx"]) == (132, 103, 159)[index], f"baseline scenario{index}: historical trace cost changed")
                require(len(b["tx"]) < len(a["tx"]), f"scenario{index}: packet reduction missing")
                if index == 0:
                    require(b["summary"]["kinds"] == [1, 1, 1, 1, 1, 2, 0] and len(b["tx"]) == 7,
                            "idle: expected only one startup transaction")
                    require(all(row[0] < 7000 for row in b["tx"]), "idle: periodic TX remains")
                    require(b["rows"] == a["rows"][:len(b["rows"])], "idle: startup value/order/time changed")
                rows.append({"scenario": index, "baseline_packets": len(a["tx"]),
                             "candidate_packets": len(b["tx"]), "baseline_summary": a["summary"],
                             "candidate_summary": b["summary"], "candidate_common_reservations": len(b["plans"]),
                             "candidate_final": b["final"],
                             "assertions": "Raw row order, deterministic intent, immutable reservation, full traced state, common future edge,40ms bound, per-role sequence,10ms deliveries, exact final/recovery witnesses; counts supplemental."})
        reports.append({"repeat": repeat, "baseline_sha256": hashlib.sha256(raw[0]).hexdigest(),
                        "candidate_sha256": hashlib.sha256(raw[1]).hexdigest(),
                        "validated_reference_repeat": 1, "scenarios": rows})
    (args.output / "results.json").write_text(json.dumps(reports, indent=2))
    print("periodic/on-demand traffic: PASS3 identical input slots; ordered behavior validated once")


if __name__ == "__main__":
    main()
