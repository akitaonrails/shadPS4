#!/usr/bin/env python3
import argparse
import array
import fcntl
import json
import os
import struct
import time
from pathlib import Path

INPUT_EVENT = struct.Struct("llHHi")
UINPUT_USER_DEV = struct.Struct("80sHHHHi" + "i" * 64 * 4)

EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0

BUS_USB = 0x03

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502

DEFAULT_UINPUT = "/dev/uinput"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay a recorded 8BitDo joystick session through uinput."
    )
    parser.add_argument("recording", type=Path, help="Recording JSON from driveclub_pad_record.py")
    parser.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier")
    parser.add_argument("--uinput", default=DEFAULT_UINPUT, help="uinput device path")
    parser.add_argument(
        "--settle-ms",
        type=int,
        default=1200,
        help="Delay after device creation so SDL can discover the virtual pad",
    )
    parser.add_argument(
        "--pre-delay-ms",
        type=int,
        default=0,
        help="Extra delay before the first replayed input event",
    )
    parser.add_argument(
        "--hold-ms",
        type=int,
        default=3000,
        help="Keep the virtual pad alive after replay so the emulator can finish processing",
    )
    parser.add_argument(
        "--tail-confirm-count",
        type=int,
        default=0,
        help="Append this many BTN_SOUTH confirm taps after the recording finishes",
    )
    parser.add_argument(
        "--tail-confirm-delay-ms",
        type=int,
        default=2000,
        help="Delay before the first appended confirm tap and between appended taps",
    )
    parser.add_argument(
        "--tail-confirm-hold-ms",
        type=int,
        default=90,
        help="Hold time for each appended confirm tap",
    )
    return parser.parse_args()


def ioctl_set_bits(fd: int, req: int, codes: list[int]) -> None:
    for code in sorted(set(codes)):
        fcntl.ioctl(fd, req, code)


def build_abs_ranges(events: list[dict]) -> tuple[list[int], list[int], list[int], list[int]]:
    mins = [0] * 64
    maxs = [0] * 64
    fuzz = [0] * 64
    flat = [0] * 64
    seen = {}
    for event in events:
        if event["type"] != EV_ABS:
            continue
        code = int(event["code"])
        value = int(event["value"])
        low, high = seen.get(code, (value, value))
        seen[code] = (min(low, value), max(high, value))
    for code, (low, high) in seen.items():
        if code >= 64:
            continue
        if low == high:
            if high > 0:
                low = 0
            else:
                low, high = -32768, 32767
        mins[code] = low
        maxs[code] = high
        flat[code] = 16 if low < 0 < high else 0
    return mins, maxs, fuzz, flat


def write_event(fd: int, ev_type: int, code: int, value: int) -> None:
    fd_write = os.write
    fd_write(fd, INPUT_EVENT.pack(0, 0, ev_type, code, value))


def write_syn(fd: int) -> None:
    write_event(fd, EV_SYN, SYN_REPORT, 0)


def tap_key(fd: int, code: int, hold_ms: int) -> None:
    write_event(fd, EV_KEY, code, 1)
    write_syn(fd)
    time.sleep(max(hold_ms, 0) / 1000.0)
    write_event(fd, EV_KEY, code, 0)
    write_syn(fd)


def main() -> int:
    args = parse_args()
    payload = json.loads(args.recording.read_text())
    events = payload["events"]
    key_codes = [int(x) for x in payload.get("key_codes", [])]
    abs_codes = [int(x) for x in payload.get("abs_codes", [])]
    absmin, absmax, absfuzz, absflat = build_abs_ranges(events)

    fd = os.open(args.uinput, os.O_WRONLY | os.O_NONBLOCK)
    try:
        ioctl_set_bits(fd, UI_SET_EVBIT, [EV_SYN, EV_KEY, EV_ABS])
        ioctl_set_bits(fd, UI_SET_KEYBIT, key_codes)
        ioctl_set_bits(fd, UI_SET_ABSBIT, abs_codes)

        name = b"8BitDo Replay Virtual Gamepad"
        packed = UINPUT_USER_DEV.pack(
            name.ljust(80, b"\0"),
            BUS_USB,
            0x2DC8,
            0x3106,
            1,
            0,
            *(absmax + absmin + absfuzz + absflat),
        )
        os.write(fd, packed)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        time.sleep(args.settle_ms / 1000.0)
        if args.pre_delay_ms > 0:
            print(
                f"[pad-replay] device ready, waiting {args.pre_delay_ms}ms before playback"
            )
            time.sleep(args.pre_delay_ms / 1000.0)

        start = time.monotonic()
        prev_dt = 0.0
        for event in events:
            dt = float(event["dt"]) / max(args.speed, 0.001)
            delay = dt - prev_dt
            if delay > 0:
                time.sleep(delay)
            write_event(fd, int(event["type"]), int(event["code"]), int(event["value"]))
            prev_dt = dt

        # Ensure final state is flushed.
        write_event(fd, EV_SYN, SYN_REPORT, 0)
        total = time.monotonic() - start
        print(
            f"[pad-replay] replayed {len(events)} events over {total:.3f}s "
            f"from {args.recording}"
        )
        if args.tail_confirm_count > 0:
            print(
                "[pad-replay] appending "
                f"{args.tail_confirm_count} confirm taps "
                f"every {args.tail_confirm_delay_ms}ms"
            )
            for idx in range(args.tail_confirm_count):
                time.sleep(args.tail_confirm_delay_ms / 1000.0)
                tap_key(fd, 304, args.tail_confirm_hold_ms)
                print(f"[pad-replay] appended confirm {idx + 1}/{args.tail_confirm_count}")
        if args.hold_ms > 0:
            print(f"[pad-replay] holding virtual pad for {args.hold_ms}ms")
            time.sleep(args.hold_ms / 1000.0)
        return 0
    finally:
        try:
            fcntl.ioctl(fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
