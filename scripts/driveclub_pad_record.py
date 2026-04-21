#!/usr/bin/env python3
import argparse
import json
import os
import select
import struct
import sys
import time
from pathlib import Path

EVENT_STRUCT = struct.Struct("llHHi")
EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0

DEFAULT_DEVICE = (
    "/dev/input/by-id/"
    "usb-8BitDo_8BitDo_Ultimate_2_Wireless_Controller_for_PC_60FB9EA43C-event-joystick"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Record one raw joystick session from the 8BitDo controller."
    )
    parser.add_argument("output", type=Path, help="Output JSON file")
    parser.add_argument("--device", default=DEFAULT_DEVICE, help="Input event device path")
    parser.add_argument(
        "--seconds",
        type=float,
        default=0.0,
        help="Optional max record time; 0 means until Ctrl-C",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    fd = os.open(args.device, os.O_RDONLY | os.O_NONBLOCK)
    start = None
    last_activity = None
    events = []
    abs_state = {}
    key_state = {}
    print(f"[pad-record] device={args.device}")
    print(f"[pad-record] writing={args.output}")
    print("[pad-record] press controller input to start; Ctrl-C to stop")
    try:
        while True:
            if args.seconds and start is not None and (time.monotonic() - start) >= args.seconds:
                break
            readable, _, _ = select.select([fd], [], [], 0.25)
            if not readable:
                continue
            chunk = os.read(fd, EVENT_STRUCT.size * 64)
            if not chunk:
                continue
            now = time.monotonic()
            for off in range(0, len(chunk), EVENT_STRUCT.size):
                packet = chunk[off : off + EVENT_STRUCT.size]
                if len(packet) != EVENT_STRUCT.size:
                    continue
                _, _, ev_type, code, value = EVENT_STRUCT.unpack(packet)
                if ev_type not in (EV_SYN, EV_KEY, EV_ABS):
                    continue
                if start is None and ev_type != EV_SYN:
                    start = now
                if start is None:
                    continue
                dt = now - start
                events.append({"dt": dt, "type": ev_type, "code": code, "value": value})
                last_activity = now
                if ev_type == EV_ABS:
                    abs_state[code] = value
                elif ev_type == EV_KEY:
                    key_state[code] = value
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)

    if not events:
        print("[pad-record] no events captured", file=sys.stderr)
        return 1

    payload = {
        "device": args.device,
        "recorded_at": time.time(),
        "events": events,
        "abs_codes": sorted(abs_state),
        "key_codes": sorted(key_state),
        "last_abs_state": abs_state,
        "last_key_state": key_state,
        "duration": events[-1]["dt"],
        "event_count": len(events),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2))
    print(
        f"[pad-record] saved {len(events)} events over {payload['duration']:.3f}s "
        f"to {args.output}"
    )
    if last_activity is not None:
        print(f"[pad-record] last-activity={last_activity - start:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
