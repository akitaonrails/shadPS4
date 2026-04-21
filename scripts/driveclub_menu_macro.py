#!/usr/bin/env python3
import argparse
import fcntl
import os
import struct
import time

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

BTN_SOUTH = 304
ABS_HAT0X = 16
ABS_RZ = 5

DEFAULT_UINPUT = "/dev/uinput"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Deterministic Driveclub menu-to-race macro through uinput."
    )
    parser.add_argument("--uinput", default=DEFAULT_UINPUT)
    parser.add_argument("--settle-ms", type=int, default=1200)
    parser.add_argument("--pre-delay-ms", type=int, default=7000)
    parser.add_argument(
        "--hold-ms",
        type=int,
        default=15000,
        help="Keep virtual pad alive after macro finishes",
    )
    parser.add_argument(
        "--x-gap-ms",
        type=int,
        default=1900,
        help="Base gap between confirm taps",
    )
    parser.add_argument(
        "--tap-ms",
        type=int,
        default=90,
        help="Press duration for X and dpad taps",
    )
    return parser.parse_args()


def ioctl_set_bits(fd: int, req: int, codes: list[int]) -> None:
    for code in sorted(set(codes)):
        fcntl.ioctl(fd, req, code)


def write_event(fd: int, ev_type: int, code: int, value: int) -> None:
    os.write(fd, INPUT_EVENT.pack(0, 0, ev_type, code, value))


def syn(fd: int) -> None:
    write_event(fd, EV_SYN, SYN_REPORT, 0)


def tap_key(fd: int, code: int, hold_ms: int) -> None:
    write_event(fd, EV_KEY, code, 1)
    syn(fd)
    time.sleep(hold_ms / 1000.0)
    write_event(fd, EV_KEY, code, 0)
    syn(fd)


def tap_hat_right(fd: int, hold_ms: int) -> None:
    write_event(fd, EV_ABS, ABS_HAT0X, 1)
    syn(fd)
    time.sleep(hold_ms / 1000.0)
    write_event(fd, EV_ABS, ABS_HAT0X, 0)
    syn(fd)


def ramp_r2(fd: int, up_ms: int = 250, hold_ms: int = 5000) -> None:
    steps = [
        20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 235, 245, 252, 255
    ]
    delay = up_ms / max(len(steps), 1) / 1000.0
    for value in steps:
        write_event(fd, EV_ABS, ABS_RZ, value)
        syn(fd)
        time.sleep(delay)
    time.sleep(hold_ms / 1000.0)
    write_event(fd, EV_ABS, ABS_RZ, 0)
    syn(fd)


def sleep_ms(ms: int) -> None:
    if ms > 0:
        time.sleep(ms / 1000.0)


def run_macro(fd: int, args: argparse.Namespace) -> None:
    print(f"[menu-macro] waiting {args.pre_delay_ms}ms before playback")
    sleep_ms(args.pre_delay_ms)

    # Based on the successful manual run in session-2, but converted to a
    # deterministic menu macro:
    # X, X, X, Right, X, X, Right, Right, X x8, then accelerate in-race.
    tap_key(fd, BTN_SOUTH, args.tap_ms)
    sleep_ms(2100)
    tap_key(fd, BTN_SOUTH, args.tap_ms)
    sleep_ms(2000)
    tap_key(fd, BTN_SOUTH, args.tap_ms)
    sleep_ms(2100)

    tap_hat_right(fd, args.tap_ms)
    sleep_ms(1050)
    tap_key(fd, BTN_SOUTH, args.tap_ms)
    sleep_ms(1750)
    tap_key(fd, BTN_SOUTH, args.tap_ms)
    sleep_ms(1650)

    tap_hat_right(fd, args.tap_ms)
    sleep_ms(1100)
    tap_hat_right(fd, args.tap_ms)
    sleep_ms(1050)

    for idx in range(8):
        tap_key(fd, BTN_SOUTH, args.tap_ms)
        if idx == 0:
            sleep_ms(2300)
        elif idx == 7:
            sleep_ms(7400)
        else:
            sleep_ms(args.x_gap_ms)

    ramp_r2(fd)


def main() -> int:
    args = parse_args()
    fd = os.open(args.uinput, os.O_WRONLY | os.O_NONBLOCK)
    try:
        ioctl_set_bits(fd, UI_SET_EVBIT, [EV_SYN, EV_KEY, EV_ABS])
        ioctl_set_bits(fd, UI_SET_KEYBIT, [BTN_SOUTH])
        ioctl_set_bits(fd, UI_SET_ABSBIT, [ABS_HAT0X, ABS_RZ])

        absmax = [0] * 64
        absmin = [0] * 64
        absfuzz = [0] * 64
        absflat = [0] * 64
        absmin[ABS_HAT0X], absmax[ABS_HAT0X] = -1, 1
        absmin[ABS_RZ], absmax[ABS_RZ] = 0, 255

        packed = UINPUT_USER_DEV.pack(
            b"Driveclub Menu Macro Pad".ljust(80, b"\0"),
            BUS_USB,
            0x2DC8,
            0x3106,
            1,
            0,
            *(absmax + absmin + absfuzz + absflat),
        )
        os.write(fd, packed)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        sleep_ms(args.settle_ms)

        run_macro(fd, args)

        if args.hold_ms > 0:
            print(f"[menu-macro] holding virtual pad for {args.hold_ms}ms")
            sleep_ms(args.hold_ms)
        return 0
    finally:
        try:
            fcntl.ioctl(fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
