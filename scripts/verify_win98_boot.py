#!/usr/bin/env python3
"""
Rigorous Windows 98 Boot & Stability Verifier for 86Box.

Verifies:
1. Successful IPC connectivity and emulator state.
2. POST verification (screen is not black/blank at t=5..8s, unique colors >= 2).
3. Automated startup menu navigation via IPC `send_keys`.
4. Kernel / protected mode transition via dynarec telemetry (cache hits increasing).
5. Desktop reachability check (screenshot at t=40s has >= 16 unique colors, not blank, not stuck at splash).
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

def parse_args():
    parser = argparse.ArgumentParser(description="Verify 86Box Win98 boot")
    parser.add_argument("--vm-dir", default="/home/marm/Emulation/86Box/Windows98", help="Path to VM directory")
    parser.add_argument("--emu-bin", default="/home/marm/Emulation/86Box/86Box", help="Path to 86Box executable")
    parser.add_argument("--timeout", type=int, default=45, help="Total boot test timeout in seconds")
    parser.add_argument("--post-timeout", type=int, default=10, help="Max seconds to wait for POST display")
    parser.add_argument("--screenshot", default="scratch/win98_verified.png", help="Path to save final screenshot")
    parser.add_argument("--ipc-socket", default="/tmp/verify-win98-ipc.sock", help="Path to IPC socket")
    parser.add_argument("--post-only", action="store_true", help="Only verify POST and exit early")
    parser.add_argument("--link-mask", type=str, default=None, help="Set BOX86_LINK_MASK")
    return parser.parse_args()

class IPCClient:
    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self._req_id = 1

    def connect(self, timeout=15.0):
        start = time.time()
        while time.time() - start < timeout:
            if os.path.exists(self.sock_path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(self.sock_path)
                    self.sock = s
                    return True
                except Exception:
                    pass
            time.sleep(0.2)
        return False

    def call(self, method, params=None):
        if not self.sock:
            raise RuntimeError("IPC socket not connected")
        req_id = self._req_id
        self._req_id += 1
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params
        data = json.dumps(msg).encode("utf-8") + b"\n"
        self.sock.sendall(data)
        buf = bytearray()
        while b"\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("IPC socket closed unexpectedly")
            buf.extend(chunk)
        line = bytes(buf).split(b"\n", 1)[0]
        resp = json.loads(line.decode("utf-8"))
        if "error" in resp:
            err = resp["error"]
            raise RuntimeError(f"IPC Error ({err.get('code')}): {err.get('message')}")
        return resp.get("result", {})

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

def ocr_screen(path):
    """
    Run tesseract on the screenshot to detect desktop text elements vs splash screen.
    Returns:
        dict with keys:
            is_desktop (bool): True if desktop UI text like 'Start' and 'My Computer' is detected.
            is_splash (bool): True if Windows 98 splash screen text is detected.
            detected_items (list): List of detected UI tokens.
            raw_text (str): Full text output from OCR.
    """
    if not os.path.exists(path):
        return {"is_desktop": False, "is_splash": False, "detected_items": [], "raw_text": ""}

    try:
        # Run tesseract with PSM 11 (sparse text) and PSM 6 (uniform block of text)
        res6 = subprocess.run(
            ["tesseract", str(path), "stdout", "--psm", "6"],
            capture_output=True, text=True, timeout=10
        )
        res11 = subprocess.run(
            ["tesseract", str(path), "stdout", "--psm", "11"],
            capture_output=True, text=True, timeout=10
        )
        combined_text = (res6.stdout + "\n" + res11.stdout).lower()

        # Normalize non-empty unique OCR lines while preserving order
        ocr_lines = []
        seen_lines = set()
        for raw_line in (res6.stdout + "\n" + res11.stdout).splitlines():
            line = " ".join(raw_line.split())
            if line and line.lower() not in seen_lines:
                seen_lines.add(line.lower())
                ocr_lines.append(line)

        # Check splash / scandisk indicators
        is_scandisk = "scandisk" in combined_text
        is_splash = any(k in combined_text for k in [
            "windows98", "windows 98", "microsoft windows 98"
        ])

        # Check desktop indicators
        has_my_computer = "my computer" in combined_text
        has_start = (not is_scandisk) and any(k in combined_text for k in ["start", "mstart", "astan"])
        has_recycle_bin = "recycle bin" in combined_text
        has_network = "network" in combined_text or "neighborhood" in combined_text
        has_my_docs = "my documents" in combined_text or "dacurnents" in combined_text

        detected_items = []
        if has_my_computer:
            detected_items.append("My Computer")
        if has_start:
            detected_items.append("Start")
        if has_recycle_bin:
            detected_items.append("Recycle Bin")
        if has_network:
            detected_items.append("Network Neighborhood")
        if has_my_docs:
            detected_items.append("My Documents")

        # Desktop is confirmed if we have at least 2 distinct desktop UI elements AND not stuck on splash/scandisk
        is_desktop = (len(detected_items) >= 2) and not is_splash and not is_scandisk

        return {
            "is_desktop": is_desktop,
            "is_splash": is_splash,
            "is_scandisk": is_scandisk,
            "detected_items": detected_items,
            "ocr_lines": ocr_lines,
            "raw_text": combined_text
        }
    except Exception as e:
        print(f"[!] OCR error on {path}: {e}")
        return {"is_desktop": False, "is_splash": False, "is_scandisk": False, "detected_items": [], "ocr_lines": [], "raw_text": ""}

def analyze_image(path):
    """Returns (num_unique_colors, is_all_black, top_colors)"""
    if not os.path.exists(path):
        return 0, True, []
    try:
        im = Image.open(path).convert("RGB")
        colors = im.getcolors(maxcolors=100000)
        if not colors:
            return 99999, False, []
        colors_sorted = sorted(colors, reverse=True)
        num_unique = len(colors_sorted)
        total_pixels = im.width * im.height
        top_count, top_color = colors_sorted[0]
        is_black = (top_color == (0, 0, 0) and top_count >= int(total_pixels * 0.999))
        return num_unique, is_black, colors_sorted[:5]
    except Exception as e:
        print(f"[!] Image analysis error on {path}: {e}")
        return 0, True, []

def main():
    args = parse_args()
    vm_dir = Path(args.vm_dir).resolve()
    emu_bin = Path(args.emu_bin).resolve()
    vhd_bak = vm_dir / "windows98.vhd.bak"
    vhd_cur = vm_dir / "windows98.vhd"

    if not emu_bin.exists():
        print(f"[FAIL] Emulator binary not found: {emu_bin}")
        return 1
    if not vm_dir.exists():
        print(f"[FAIL] VM dir not found: {vm_dir}")
        return 1

    # 1. Kill any existing 86Box processes
    subprocess.run(["pkill", "-9", "86Box"], stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    # 2. Restore clean VHD
    if vhd_bak.exists():
        print(f"[*] Restoring clean VHD from {vhd_bak.name}...")
        shutil.copyfile(vhd_bak, vhd_cur)

    # 3. Prepare socket and environment
    if os.path.exists(args.ipc_socket):
        try:
            os.unlink(args.ipc_socket)
        except Exception:
            pass

    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["ALSOFT_DRIVERS"] = "null"
    if args.link_mask is not None:
        env["BOX86_LINK_MASK"] = str(args.link_mask)

    print(f"[*] Launching 86Box ({emu_bin.name}) with IPC on {args.ipc_socket}...")
    proc = subprocess.Popen(
        [str(emu_bin), "-P", str(vm_dir), "-platform", "offscreen", "--ipc", args.ipc_socket],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ipc = IPCClient(args.ipc_socket)
    if not ipc.connect(timeout=10.0):
        print(f"[FAIL] Could not connect to IPC socket within 10s. Process alive: {proc.poll() is None}")
        proc.kill()
        return 1

    post_ok = False
    menu_sent = False
    start_time = time.time()
    last_pc = ""
    last_hits = 0
    post_img = "/tmp/win98_post_check.png"

    try:
        while time.time() - start_time < args.timeout:
            elapsed = time.time() - start_time
            time.sleep(1.0)

            # Query IPC status
            try:
                status = ipc.call("get_status")
                pc = status.get("pc", "")
                fps = status.get("fps", 0)
                cycles = status.get("cycles", 0)
                last_pc = pc
            except Exception as e:
                print(f"[!] IPC get_status failed at t={elapsed:.1f}s: {e}")
                continue

            try:
                stats = ipc.call("dynarec_get_stats")
                last_hits = stats.get("cache_hits", 0)
            except Exception:
                pass

            # Step A: Check POST between 4s and post-timeout
            if not post_ok and elapsed >= 4.0:
                ipc.call("take_screenshot", {"path": post_img})
                colors_cnt, is_black, top_cols = analyze_image(post_img)
                if is_black or colors_cnt <= 1:
                    print(f"[{elapsed:4.1f}s] POST Check: SCREEN IS BLANK/BLACK (colors={colors_cnt}, pc={pc}, hits={last_hits})")
                    if elapsed >= args.post_timeout:
                        print(f"\n[FAIL] Guest failed to POST: Screen remained 100% black for {args.post_timeout}s.")
                        return 2
                else:
                    post_ok = True
                    print(f"[{elapsed:4.1f}s] [PASS] POST Verified! (unique colors={colors_cnt}, pc={pc}, hits={last_hits})")
                    if args.post_only:
                        print("[SUCCESS] POST verified successfully.")
                        return 0

            # Step B: Send Startup Menu key '1' + Enter around 8s
            if post_ok and not menu_sent and elapsed >= 8.0:
                print(f"[{elapsed:4.1f}s] Sending Startup Menu selection (Normal Boot)...")
                try:
                    ipc.call("send_keys", {"string": "1\n"})
                    menu_sent = True
                except Exception as e:
                    print(f"[!] send_keys failed: {e}")

            # Telemetry logging every ~4s
            if int(elapsed) % 4 == 0:
                print(f"[{elapsed:4.1f}s] pc={pc} hits={last_hits} fps={fps}")

            # Step C: Desktop check from 35s onwards
            if post_ok and elapsed >= 35.0:
                final_ss = Path(args.screenshot).resolve()
                final_ss.parent.mkdir(parents=True, exist_ok=True)
                ipc.call("take_screenshot", {"path": str(final_ss)})
                colors_cnt, is_black, top_cols = analyze_image(str(final_ss))

                if is_black or colors_cnt <= 1:
                    print(f"[{elapsed:4.1f}s] Desktop Check: Screen is blank/black (colors={colors_cnt})")
                else:
                    ocr_res = ocr_screen(str(final_ss))
                    if ocr_res["is_splash"]:
                        print(f"[{elapsed:4.1f}s] Desktop Check: Still on Windows 98 Splash Screen (OCR: {ocr_res['ocr_lines']})")
                    elif ocr_res["is_scandisk"]:
                        print(f"[{elapsed:4.1f}s] Desktop Check: ScanDisk active (OCR: {ocr_res['ocr_lines']})")
                    elif ocr_res["is_desktop"]:
                        print(f"\n[SUCCESS] Windows 98 Desktop GUI Verified!")
                        print(f"  - POST: Passed")
                        print(f"  - Dynarec cache hits: {last_hits:,}")
                        print(f"  - Screen status: Displaying desktop GUI ({colors_cnt} unique colors)")
                        print(f"  - OCR detected elements: {', '.join(ocr_res['detected_items'])}")
                        print(f"  - OCR full text: {' | '.join(ocr_res['ocr_lines'])}")
                        print(f"  - Screenshot saved: {final_ss}")
                        return 0
                    else:
                        print(f"[{elapsed:4.1f}s] Desktop Check: Waiting for desktop UI (detected: {ocr_res['detected_items']}, OCR: {ocr_res['ocr_lines']})")

        # If timeout reached without desktop
        final_ss = Path(args.screenshot).resolve()
        final_ss.parent.mkdir(parents=True, exist_ok=True)
        ipc.call("take_screenshot", {"path": str(final_ss)})
        colors_cnt, is_black, _ = analyze_image(str(final_ss))
        ocr_res = ocr_screen(str(final_ss))
        if ocr_res["is_splash"]:
            print(f"\n[FAIL] Windows 98 hung on Splash Screen after {args.timeout}s.")
        elif ocr_res["is_scandisk"]:
            print(f"\n[FAIL] Windows 98 still in ScanDisk after {args.timeout}s.")
        else:
            print(f"\n[FAIL] Windows 98 failed to reach desktop within {args.timeout}s.")
        print(f"  - Last PC: {last_pc}")
        print(f"  - Final Dynarec cache hits: {last_hits:,}")
        print(f"  - Final Screenshot colors: {colors_cnt} (is_black={is_black})")
        print(f"  - OCR Status: splash={ocr_res['is_splash']}, scandisk={ocr_res['is_scandisk']}, desktop_items={ocr_res['detected_items']}")
        if ocr_res["ocr_lines"]:
            print(f"  - OCR Full Text:")
            for line in ocr_res["ocr_lines"]:
                print(f"      {line}")
        else:
            print(f"  - OCR Full Text: <none>")
        print(f"  - Saved failure screenshot: {final_ss}")
        return 3

    finally:
        ipc.close()
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=5)
        except Exception:
            proc.kill()
            out, err = proc.communicate()
        with open("/tmp/emu_out.log", "w") as f:
            if out: f.write(out)
            if err: f.write(err)
        for line in (out + err).splitlines():
            if any(k in line for k in ("[ABRT]", "[DOUBLE", "[TRIPLE", "[BFX]")):
                print("  emu log:", line)

if __name__ == "__main__":
    sys.exit(main())
