#!/usr/bin/env python3
"""
86Box IPC Client - Control and inspect 86Box via local socket JSON-RPC.

Supports:
  - Taking non-invasive screenshots (without GDB attachment)
  - Querying VM status (CPU, machine, state, FPS, cycles)
  - Emulation control (pause, resume, reset, stop, CAD)
  - Guest input injection (keystrokes, mouse moves/clicks)
  - Dynamic Dynarec block profiling (start/stop dump, link mask, flush)
  - Dynamic dump triggers (arm breakpoint on executable name, PC range, or byte pattern)
"""

import sys
import os
import socket
import json
import argparse
import time

DEFAULT_SOCKET_UNIX = "/tmp/86box-ipc-latest.sock"

def get_default_socket():
    env_sock = os.environ.get("BOX86_IPC_SOCKET")
    if env_sock:
        return env_sock
    if sys.platform == "win32":
        return r"\\.\pipe\86box-ipc-latest"
    return DEFAULT_SOCKET_UNIX

class IPCClient:
    def __init__(self, socket_path=None, timeout=15.0):
        self.socket_path = socket_path or get_default_socket()
        self.timeout = timeout
        self.sock = None
        self._req_id = 1

    def connect(self):
        if sys.platform == "win32":
            # Windows Named Pipe
            self.sock = open(self.socket_path, "r+b", buffering=0)
        else:
            # Unix Domain Socket
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            try:
                self.sock.connect(self.socket_path)
            except Exception as e:
                raise ConnectionError(f"Failed to connect to 86Box IPC socket at '{self.socket_path}': {e}")

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def call(self, method, params=None):
        if not self.sock:
            self.connect()

        req_id = self._req_id
        self._req_id += 1

        payload = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method
        }
        if params is not None:
            payload["params"] = params

        data = json.dumps(payload).encode("utf-8") + b"\n"

        if sys.platform == "win32":
            self.sock.write(data)
            line = self.sock.readline()
        else:
            self.sock.sendall(data)
            buf = bytearray()
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf.extend(chunk)
                if b"\n" in buf:
                    break
            line = bytes(buf).split(b"\n", 1)[0]

        if not line:
            raise RuntimeError("Empty response received from 86Box IPC server")

        resp = json.loads(line.decode("utf-8"))
        if "error" in resp:
            err = resp["error"]
            raise RuntimeError(f"IPC Error ({err.get('code')}): {err.get('message')}")
        return resp.get("result", {})


def cmd_screenshot(client, args):
    params = {}
    if args.path:
        params["path"] = os.path.abspath(args.path)
    if args.monitor is not None:
        params["monitor"] = args.monitor

    res = client.call("take_screenshot", params)
    print(f"Screenshot saved: {res.get('path')} ({res.get('width')}x{res.get('height')})")


def cmd_status(client, args):
    res = client.call("get_status")
    print("86Box VM Status:")
    for k, v in res.items():
        print(f"  {k:15}: {v}")


def cmd_pause(client, args):
    res = client.call("pause")
    print(f"VM State: {res.get('state')}")


def cmd_resume(client, args):
    res = client.call("resume")
    print(f"VM State: {res.get('state')}")


def cmd_reset(client, args):
    res = client.call("reset", {"type": "hard" if args.hard else "soft"})
    print(f"VM Reset: {res.get('status')}")


def cmd_cad(client, args):
    res = client.call("ctrl_alt_del")
    print(f"Ctrl+Alt+Del: {res.get('status')}")


def cmd_stop(client, args):
    res = client.call("stop")
    print(f"Stop: {res.get('status')}")


def cmd_type(client, args):
    res = client.call("send_keys", {"string": args.text})
    print(f"Injected {res.get('keys_sent')} keystrokes.")


def cmd_mouse(client, args):
    res = client.call("send_mouse", {"dx": args.dx, "dy": args.dy, "buttons": args.buttons})
    print(f"Mouse update: {res.get('status')}")


def cmd_dump_start(client, args):
    params = {"path": os.path.abspath(args.path)}
    if args.min_exec:
        params["min_exec"] = args.min_exec
    res = client.call("dynarec_start_dump", params)
    print(f"Dynarec dump started -> {res.get('path')}")


def cmd_dump_stop(client, args):
    res = client.call("dynarec_stop_dump")
    print(f"Dynarec dump stopped: {res.get('status')}")


def cmd_arm_trigger(client, args):
    params = {
        "type": args.type,
        "output_path": os.path.abspath(args.output) if args.output else "",
        "take_screenshot": args.screenshot,
    }
    if args.pattern:
        params["pattern"] = args.pattern
    if args.pc_start:
        params["pc_start"] = int(args.pc_start, 0)
    if args.pc_end:
        params["pc_end"] = int(args.pc_end, 0)
    if args.bytes:
        params["byte_pattern"] = args.bytes
    if args.max_blocks:
        params["max_blocks"] = args.max_blocks

    res = client.call("dynarec_arm_trigger", params)
    print(f"Dynarec trigger armed: type={res.get('type')}, output={res.get('output')}")


def cmd_disarm_trigger(client, args):
    res = client.call("dynarec_disarm_trigger")
    print(f"Dynarec trigger disarmed: {res.get('status')}")


def cmd_raw(client, args):
    payload = json.loads(args.json_string)
    method = payload.get("method")
    params = payload.get("params")
    res = client.call(method, params)
    print(json.dumps(res, indent=2))


def main():
    parser = argparse.ArgumentParser(description="86Box IPC Control & Telemetry Client")
    parser.add_argument("-s", "--socket", help="Path to 86Box IPC socket (default: autodetect or /tmp/86box-ipc-latest.sock)")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # screenshot
    p_shot = subparsers.add_parser("screenshot", help="Take a non-invasive screenshot")
    p_shot.add_argument("path", nargs="?", help="Destination PNG file path")
    p_shot.add_argument("-m", "--monitor", type=int, default=0, help="Monitor index (default: 0)")
    p_shot.set_defaults(func=cmd_screenshot)

    # status
    p_status = subparsers.add_parser("status", help="Get VM execution status")
    p_status.set_defaults(func=cmd_status)

    # pause / resume
    p_pause = subparsers.add_parser("pause", help="Pause VM emulation")
    p_pause.set_defaults(func=cmd_pause)
    p_resume = subparsers.add_parser("resume", help="Resume VM emulation")
    p_resume.set_defaults(func=cmd_resume)

    # reset
    p_reset = subparsers.add_parser("reset", help="Reset the VM")
    p_reset.add_argument("--hard", action="store_true", help="Perform hard reset instead of soft")
    p_reset.set_defaults(func=cmd_reset)

    # cad
    p_cad = subparsers.add_parser("cad", help="Send Ctrl+Alt+Del")
    p_cad.set_defaults(func=cmd_cad)

    # stop
    p_stop = subparsers.add_parser("stop", help="Request VM shutdown")
    p_stop.set_defaults(func=cmd_stop)

    # type
    p_type = subparsers.add_parser("type", help="Inject keyboard text")
    p_type.add_argument("text", help="Text string to type into guest")
    p_type.set_defaults(func=cmd_type)

    # mouse
    p_mouse = subparsers.add_parser("mouse", help="Inject mouse movement and buttons")
    p_mouse.add_argument("dx", type=int, help="Relative X movement")
    p_mouse.add_argument("dy", type=int, help="Relative Y movement")
    p_mouse.add_argument("-b", "--buttons", type=int, default=0, help="Button bitmask (1=L, 2=R, 4=M)")
    p_mouse.set_defaults(func=cmd_mouse)

    # dump-start / dump-stop
    p_dstart = subparsers.add_parser("dump-start", help="Start dynamic binary block dumping (.86bdump)")
    p_dstart.add_argument("path", help="Path to destination .86bdump file")
    p_dstart.add_argument("--min-exec", type=int, default=0, help="Minimum execution count filter")
    p_dstart.set_defaults(func=cmd_dump_start)

    p_dstop = subparsers.add_parser("dump-stop", help="Stop binary block dumping")
    p_dstop.set_defaults(func=cmd_dump_stop)

    # arm-trigger / disarm-trigger
    p_arm = subparsers.add_parser("arm-trigger", help="Arm dynamic dump trigger on code pattern or exec")
    p_arm.add_argument("--type", choices=["exec", "pc", "pattern"], required=True, help="Trigger condition type")
    p_arm.add_argument("-p", "--pattern", help="Substring pattern for exec trigger (e.g. PINBALL, SCANDISK)")
    p_arm.add_argument("--pc-start", help="Start of linear PC range (hex e.g. 0x00401000)")
    p_arm.add_argument("--pc-end", help="End of linear PC range (hex)")
    p_arm.add_argument("--bytes", help="Hex byte sequence for pattern trigger (e.g. '55 89 e5')")
    p_arm.add_argument("-o", "--output", help="Output .86bdump path once triggered")
    p_arm.add_argument("--max-blocks", type=int, default=0, help="Auto-stop after dumping N blocks")
    p_arm.add_argument("--screenshot", action="store_true", help="Snap screenshot when trigger fires")
    p_arm.set_defaults(func=cmd_arm_trigger)

    p_disarm = subparsers.add_parser("disarm-trigger", help="Disarm dynamic trigger")
    p_disarm.set_defaults(func=cmd_disarm_trigger)

    # raw
    p_raw = subparsers.add_parser("raw", help="Send raw JSON-RPC string")
    p_raw.add_argument("json_string", help="JSON string e.g. '{\"method\":\"get_status\"}'")
    p_raw.set_defaults(func=cmd_raw)

    args = parser.parse_args()

    client = IPCClient(socket_path=args.socket)
    try:
        args.func(client, args)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()

if __name__ == "__main__":
    main()
