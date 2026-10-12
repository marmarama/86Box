#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""86Box Automated Full-System Dynarec Benchmarking Harness.

Runs guest DOS benchmarks under 86Box headlessly, captures real-time output
redirected through COM1 (serial) to a host log file, and compares old vs. new dynarec.
"""

from __future__ import annotations

import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional


COLOR_RESET = "\033[0m"
COLOR_BOLD = "\033[1m"
COLOR_GREEN = "\033[32m"
COLOR_RED = "\033[31m"
COLOR_CYAN = "\033[36m"
COLOR_YELLOW = "\033[33m"

COMPLETION_MARKER = "=== BENCHMARK SUITE COMPLETE ==="


def format_delta(old_val: float, new_val: float, higher_is_better: bool = True) -> str:
    """Format a percentage delta with color indication."""
    if old_val == 0:
        return "N/A"
    diff = ((new_val - old_val) / old_val) * 100.0
    improved = diff > 0 if higher_is_better else diff < 0
    color = COLOR_GREEN if improved else (COLOR_RED if abs(diff) > 1.0 else COLOR_YELLOW)
    sign = "+" if diff > 0 else ""
    return f"{color}{sign}{diff:.1f}%{COLOR_RESET}"


def get_quake_log_len(vm_dir: Optional[Path]) -> int:
    """Get the current character length of QCONSOLE.LOG in the VHD."""
    if not vm_dir:
        return 0
    vhd_path = vm_dir / "benchmark-c.vhd"
    if not vhd_path.exists():
        return 0
    try:
        out = subprocess.check_output(
            ["7z", "e", "-so", str(vhd_path), "bench/quake/ID1/QCONSOLE.LOG"],
            stderr=subprocess.DEVNULL,
            timeout=2,
        ).decode("latin1", errors="replace")
        return len(out)
    except Exception:
        return 0


def check_quake_log(vm_dir: Optional[Path], min_offset: int = 0) -> Optional[Dict[str, Any]]:
    """Inspect QCONSOLE.LOG from the VHD image if available."""
    if not vm_dir:
        return None
    vhd_path = vm_dir / "benchmark-c.vhd"
    if not vhd_path.exists():
        return None
    try:
        out = subprocess.check_output(
            ["7z", "e", "-so", str(vhd_path), "bench/quake/ID1/QCONSOLE.LOG"],
            stderr=subprocess.DEVNULL,
            timeout=2,
        ).decode("latin1", errors="replace")
        target_text = out[min_offset:] if min_offset > 0 else out
        matches = list(re.finditer(r"(\d+)\s+frames\s+([0-9.]+)\s+seconds\s+([0-9.]+)\s+fps", target_text, re.IGNORECASE))
        if matches:
            m = matches[-1]
            return {
                "quake_frames": int(m.group(1)),
                "quake_seconds": float(m.group(2)),
                "quake_fps": float(m.group(3)),
            }
    except Exception:
        pass
    return None


def parse_serial_log(log_path: Path, vm_dir: Optional[Path] = None) -> Dict[str, Any]:
    """Parse benchmark scores from the guest serial (COM1) output log."""
    if not log_path.exists():
        return {}

    content = log_path.read_text(encoding="latin1", errors="replace")
    results: Dict[str, Any] = {}

    # Extract test sections if present
    sections: Dict[str, str] = {}
    tokens = re.split(r"\[START\]\s+([A-Za-z0-9_]+)", content, flags=re.IGNORECASE)
    for i in range(1, len(tokens), 2):
        sec_name = tokens[i].lower()
        sec_body = tokens[i + 1] if i + 1 < len(tokens) else ""
        tkey = "doom" if "doom" in sec_name else ("quake" if "quake" in sec_name else ("pcpbench" if "pcp" in sec_name else ("dhrystone" if "dhry" in sec_name else sec_name)))
        sections[tkey] = sec_body

    # 1. Quake Timedemo (e.g. "3692 frames 42.1 seconds 87.7 fps")
    quake_text = sections.get("quake", content)
    quake_matches = list(re.finditer(r"(\d+)\s+frames\s+([0-9.]+)\s+seconds\s+([0-9.]+)\s+fps", quake_text, re.IGNORECASE))
    if quake_matches:
        qm = quake_matches[-1]
        results["quake_frames"] = int(qm.group(1))
        results["quake_seconds"] = float(qm.group(2))
        results["quake_fps"] = float(qm.group(3))
    elif vm_dir:
        q_disk = check_quake_log(vm_dir)
        if q_disk:
            results.update(q_disk)

    # 2. Doom Timedemo (e.g. "timed 2134 gametics in 1234 realtics" or "realtics = 1234")
    doom_text = sections.get("doom", content)
    doom_match = re.search(r"timed\s+(\d+)\s+gametics\s+in\s+(\d+)\s+realtics", doom_text, re.IGNORECASE)
    if doom_match:
        gametics = int(doom_match.group(1))
        realtics = int(doom_match.group(2))
        fps = (gametics * 35.0) / realtics if realtics > 0 else 0.0
        results["doom_fps"] = round(fps, 2)
        results["doom_realtics"] = realtics
    else:
        realtics_match = re.search(r"realtics\s*=\s*(\d+)", doom_text, re.IGNORECASE)
        if realtics_match:
            realtics = int(realtics_match.group(1))
            fps = (2134 * 35.0) / realtics if realtics > 0 else 0.0
            results["doom_fps"] = round(fps, 2)
            results["doom_realtics"] = realtics

    # 3. PCPBench (e.g. "Frame rate: 58.3 fps" or "58.3 fps")
    pcp_text = sections.get("pcpbench", content)
    pcp_match = re.search(r"([0-9.]+)\s*(?:fps|frames/sec)", pcp_text, re.IGNORECASE)
    if pcp_match and "quake_fps" not in pcp_text:
        results["pcpbench_fps"] = float(pcp_match.group(1))

    # 4. Dhrystone 2.1 (e.g. "Dhrystones per Second: 450000" or "VAX MIPS rating = 256.1")
    dhry_text = sections.get("dhrystone", content)
    dhry_match = re.search(r"Dhrystones\s+(?:per\s+Second|/\s*second):\s*([0-9.]+)", dhry_text, re.IGNORECASE)
    if dhry_match:
        results["dhrystones_per_sec"] = float(dhry_match.group(1))
    vax_match = re.search(r"VAX\s+MIPS\s*(?:rating)?\s*[=:]\s*([0-9.]+)", dhry_text, re.IGNORECASE)
    if vax_match:
        results["dhrystone_vax_mips"] = float(vax_match.group(1))

    results["completed"] = COMPLETION_MARKER in content or "quake_fps" in results
    return results


def parse_perf_stat(output: str) -> Dict[str, float]:
    """Parse hardware counter metrics from perf stat stderr output."""
    metrics: Dict[str, float] = {}
    for line in output.splitlines():
        line = line.strip()
        # Look for "123,456,789 instructions"
        if "instructions" in line:
            m = re.match(r"([0-9,]+)\s+instructions", line)
            if m:
                metrics["instructions"] = float(m.group(1).replace(",", ""))
        elif "cycles" in line and "frequency" not in line:
            m = re.match(r"([0-9,]+)\s+cycles", line)
            if m:
                metrics["cycles"] = float(m.group(1).replace(",", ""))
        elif "L1-dcache-load-misses" in line:
            m = re.match(r"([0-9,]+)\s+L1-dcache-load-misses", line)
            if m:
                metrics["l1_dcache_misses"] = float(m.group(1).replace(",", ""))
        elif "branch-misses" in line:
            m = re.match(r"([0-9,]+)\s+branch-misses", line)
            if m:
                metrics["branch_misses"] = float(m.group(1).replace(",", ""))
    return metrics


def capture_screenshot(out_path: Path) -> bool:
    """Capture a host screen/window screenshot using spectacle or similar tool."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if shutil.which("spectacle"):
            res = subprocess.run(
                ["spectacle", "-b", "-n", "-o", str(out_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
            return res.returncode == 0
    except Exception:
        pass
    return False


def handle_breakpoint(
    proc: subprocess.Popen,
    breakpoint_type: str,
    test_name: str,
    results_file: Path,
    interactive: bool = False,
    screenshot_dir: Optional[Path] = None,
) -> bool:
    """Pause emulator with SIGSTOP, display telemetry, take screenshot, and prompt if interactive.
    Returns True to resume with SIGCONT, False to terminate the VM.
    """
    try:
        proc.send_signal(signal.SIGSTOP)
    except Exception as e:
        print(f"  {COLOR_RED}Failed to send SIGSTOP: {e}{COLOR_RESET}")

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    print("\n" + "=" * 76)
    print(f"{COLOR_YELLOW}{COLOR_BOLD}[BREAKPOINT TRIGGERED]{COLOR_RESET} Phase: {COLOR_BOLD}{breakpoint_type.upper()}{COLOR_RESET} | Test: {COLOR_CYAN}{COLOR_BOLD}{test_name.upper()}{COLOR_RESET}")
    print(f"Emulator process (PID {proc.pid}) paused ({COLOR_BOLD}SIGSTOP{COLOR_RESET}).")

    if screenshot_dir:
        clean_type = breakpoint_type.replace(" ", "_").replace("(", "").replace(")", "").replace("/", "_")
        ss_path = screenshot_dir / f"bp_{test_name}_{clean_type}_{timestamp}.png"
        if capture_screenshot(ss_path):
            print(f"Screenshot captured: {COLOR_GREEN}{ss_path}{COLOR_RESET}")

    if results_file.exists():
        lines = [l for l in results_file.read_text(encoding="latin1", errors="replace").splitlines() if l.strip()]
        tail = lines[-10:] if len(lines) > 10 else lines
        print("-" * 76)
        print("Recent Serial Telemetry:")
        for l in tail:
            print(f"  {COLOR_CYAN}|{COLOR_RESET} {l}")
        print("-" * 76)

    should_continue = True
    if interactive:
        while True:
            try:
                ans = input(f"{COLOR_BOLD}[c]ontinue, [s]creenshot, [t]ail log, [q]uit & collect results? {COLOR_RESET}").strip().lower()
            except (EOFError, KeyboardInterrupt):
                ans = 'q'

            if ans in ('c', ''):
                should_continue = True
                break
            elif ans == 's':
                if screenshot_dir:
                    new_ss = screenshot_dir / f"bp_{test_name}_{time.strftime('%Y%m%d_%H%M%S')}.png"
                    if capture_screenshot(new_ss):
                        print(f"New screenshot saved: {new_ss}")
            elif ans == 't':
                if results_file.exists():
                    print("\n--- Full Serial Log ---")
                    print(results_file.read_text(encoding="latin1", errors="replace"))
                    print("-----------------------\n")
            elif ans == 'q':
                should_continue = False
                break
            else:
                print("Options: 'c' to continue, 's' to screenshot, 't' to tail, 'q' to quit.")
    else:
        print("Non-interactive mode: resuming execution after breakpoint logging...")
        time.sleep(1.0)

    if should_continue:
        print(f"Resuming emulator ({COLOR_GREEN}SIGCONT{COLOR_RESET})...\n")
        try:
            proc.send_signal(signal.SIGCONT)
        except Exception:
            pass
        return True
    else:
        print(f"Stopping emulator run early on user request...")
        return False


def run_instance(
    emulator_bin: Path,
    vm_dir: Path,
    results_file: Path,
    timeout: float = 180.0,
    roms_dir: Optional[Path] = None,
    use_perf: bool = False,
    headless: bool = True,
    break_before: Optional[List[str]] = None,
    break_after: Optional[List[str]] = None,
    stop_after: Optional[str] = None,
    per_test_timeout: float = 60.0,
    interactive: bool = False,
    screenshot_dir: Optional[Path] = None,
) -> Dict[str, Any]:
    """Execute a single run of 86Box with real-time telemetry, breakpoints, and hang detection."""
    if results_file.exists():
        results_file.unlink()

    env = dict(os.environ)
    if headless:
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["ALSOFT_DRIVERS"] = "null"

    cmd: List[str] = []
    if use_perf:
        cmd.extend([
            "perf", "stat",
            "-e", "instructions,cycles,L1-dcache-load-misses,branch-misses"
        ])

    cmd.append(str(emulator_bin))
    cmd.extend(["-P", str(vm_dir)])
    if roms_dir:
        cmd.extend(["-R", str(roms_dir)])

    log_path = vm_dir / "86box_run.log"
    cmd.extend(["-L", str(log_path)])

    print(f"  {COLOR_CYAN}Running:{COLOR_RESET} {' '.join(cmd)}")
    start_time = time.monotonic()

    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    break_before_set = {t.strip().lower() for t in (break_before or []) if t.strip()}
    break_after_set = {t.strip().lower() for t in (break_after or []) if t.strip()}
    stop_after_target = stop_after.strip().lower() if stop_after else None

    started_tests = set()
    finished_tests = set()
    current_test: Optional[str] = None
    current_test_start: Optional[float] = None
    completed = False
    quake_log_start_len = 0

    processed_lines = 0
    should_terminate = False

    try:
        while proc.poll() is None:
            now = time.monotonic()
            if now - start_time > timeout:
                print(f"  {COLOR_RED}Timeout reached ({timeout}s)! Terminating...{COLOR_RESET}")
                proc.terminate()
                break

            # Poll results file for test lifecycle and completion
            if results_file.exists():
                text = results_file.read_text(encoding="latin1", errors="replace")

                # Check completion marker
                if COMPLETION_MARKER in text:
                    completed = True
                    time.sleep(1.5)
                    if proc.poll() is None:
                        proc.terminate()
                    break

                all_lines = text.splitlines()
                new_lines = all_lines[processed_lines:]
                processed_lines = len(all_lines)

                for line in new_lines:
                    # Match [START] <test>
                    start_match = re.search(r"\[START\]\s+([A-Za-z0-9_]+)", line, re.IGNORECASE)
                    if start_match:
                        raw_name = start_match.group(1).lower()
                        tname = "doom" if "doom" in raw_name else ("quake" if "quake" in raw_name else ("pcpbench" if "pcp" in raw_name else ("dhrystone" if "dhry" in raw_name else raw_name)))

                        # Check for unexpected reboot
                        if tname in finished_tests:
                            print(f"\n  {COLOR_RED}{COLOR_BOLD}[REBOOT / TRIPLE FAULT DETECTED]{COLOR_RESET} Test '{tname}' re-executed after completion!")
                            cont = handle_breakpoint(proc, "reboot_detected", tname, results_file, interactive, screenshot_dir)
                            if not cont:
                                proc.terminate()
                                should_terminate = True
                                break

                        if tname not in started_tests:
                            started_tests.add(tname)
                            current_test = tname
                            current_test_start = now
                            if tname == "quake":
                                quake_log_start_len = get_quake_log_len(vm_dir)
                            if tname in break_before_set:
                                cont = handle_breakpoint(proc, "before", tname, results_file, interactive, screenshot_dir)
                                if not cont:
                                    proc.terminate()
                                    should_terminate = True
                                    break
                                current_test_start = time.monotonic()

                    # Match [FINISH] <test>
                    finish_match = re.search(r"\[FINISH\]\s+([A-Za-z0-9_]+)", line, re.IGNORECASE)
                    if finish_match:
                        raw_name = finish_match.group(1).lower()
                        tname = "doom" if "doom" in raw_name else ("quake" if "quake" in raw_name else ("pcpbench" if "pcp" in raw_name else ("dhrystone" if "dhry" in raw_name else raw_name)))
                        if tname not in finished_tests:
                            finished_tests.add(tname)
                            if current_test == tname:
                                current_test = None
                                current_test_start = None

                            if stop_after_target and tname == stop_after_target:
                                print(f"\n  {COLOR_GREEN}{COLOR_BOLD}[STOP-AFTER HIT]{COLOR_RESET} Finished target test '{tname}'. Stopping run.")
                                completed = True
                                time.sleep(1.0)
                                proc.terminate()
                                should_terminate = True
                                break

                            if tname in break_after_set:
                                cont = handle_breakpoint(proc, "after", tname, results_file, interactive, screenshot_dir)
                                if not cont:
                                    proc.terminate()
                                    should_terminate = True
                                    break

                if should_terminate:
                    break

            # Quake special handling: poll QCONSOLE.LOG since Quake loops demos without exiting
            if current_test == "quake":
                q_res = check_quake_log(vm_dir, min_offset=quake_log_start_len)
                if q_res:
                    print(f"\n  {COLOR_GREEN}{COLOR_BOLD}[QUAKE TIMEDEMO COMPLETED]{COLOR_RESET} {q_res['quake_frames']} frames in {q_res['quake_seconds']}s = {COLOR_CYAN}{COLOR_BOLD}{q_res['quake_fps']} FPS{COLOR_RESET}")
                    finished_tests.add("quake")
                    current_test = None
                    current_test_start = None
                    completed = True
                    time.sleep(1.0)
                    proc.terminate()
                    break

            # Hang detection: check per-test timeout
            if current_test and current_test_start and per_test_timeout > 0:
                if (now - current_test_start) > per_test_timeout:
                    print(f"\n  {COLOR_RED}{COLOR_BOLD}[PER-TEST TIMEOUT]{COLOR_RESET} Test '{current_test}' exceeded {per_test_timeout}s without completing!")
                    cont = handle_breakpoint(proc, f"timeout ({per_test_timeout}s)", current_test, results_file, interactive, screenshot_dir)
                    if not cont or not interactive:
                        proc.terminate()
                        break
                    else:
                        current_test_start = time.monotonic()

            time.sleep(0.5)

        try:
            stdout, stderr = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=2)
    except Exception as e:
        proc.kill()
        stdout, stderr = proc.communicate()
        print(f"  {COLOR_RED}Error during execution: {e}{COLOR_RESET}")

    wall_clock = time.monotonic() - start_time

    perf_data = parse_perf_stat(stderr) if use_perf else {}
    guest_results = parse_serial_log(results_file, vm_dir)

    return {
        "wall_clock_seconds": round(wall_clock, 2),
        "return_code": proc.returncode,
        "guest_results": guest_results,
        "perf_data": perf_data,
        "completed": completed or guest_results.get("completed", False),
    }


def inject_guest_files(disk_image: Path, script_dir: Path) -> bool:
    """Inject AUTOEXEC.BAT and POWEROFF.COM into a FAT virtual disk using mtools."""
    if not shutil.which("mcopy"):
        print(f"{COLOR_YELLOW}mcopy not found in PATH; skipping disk injection.{COLOR_RESET}")
        return False

    autoexec = script_dir / "AUTOEXEC.BAT"
    poweroff = script_dir / "POWEROFF.COM"

    if not autoexec.exists() or not poweroff.exists():
        print(f"{COLOR_RED}AUTOEXEC.BAT or POWEROFF.COM missing from {script_dir}{COLOR_RESET}")
        return False

    try:
        # Copy to root of disk image (both AUTOEXEC.BAT and FDAUTO.BAT for MS-DOS / FreeDOS compatibility)
        subprocess.run(["mcopy", "-o", "-i", str(disk_image), str(autoexec), "::AUTOEXEC.BAT"], check=True)
        subprocess.run(["mcopy", "-o", "-i", str(disk_image), str(autoexec), "::FDAUTO.BAT"], check=True)
        subprocess.run(["mcopy", "-o", "-i", str(disk_image), str(poweroff), "::POWEROFF.COM"], check=True)
        print(f"{COLOR_GREEN}Successfully injected AUTOEXEC.BAT, FDAUTO.BAT, and POWEROFF.COM into {disk_image.name}{COLOR_RESET}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"{COLOR_RED}Failed to inject files into disk: {e}{COLOR_RESET}")
        return False


def print_comparison_table(old_res: Dict[str, Any], new_res: Dict[str, Any]):
    """Print a clean comparison report between the two runs."""
    old_g = old_res.get("guest_results", {})
    new_g = new_res.get("guest_results", {})
    old_p = old_res.get("perf_data", {})
    new_p = new_res.get("perf_data", {})

    print("\n" + "=" * 80)
    print(f"{COLOR_BOLD}86Box Dynarec Full-System Benchmark Comparison{COLOR_RESET}")
    print("=" * 80)
    print(f"{'Metric':<30} | {'Old Dynarec':<15} | {'New Dynarec':<15} | {'Delta':<10}")
    print("-" * 80)

    # 1. Guest Metrics
    if "quake_fps" in old_g or "quake_fps" in new_g:
        q_old = old_g.get("quake_fps", 0.0)
        q_new = new_g.get("quake_fps", 0.0)
        print(f"{'Quake 1 (demo1) FPS':<30} | {q_old:<15.2f} | {q_new:<15.2f} | {format_delta(q_old, q_new, True)}")

    if "doom_fps" in old_g or "doom_fps" in new_g:
        d_old = old_g.get("doom_fps", 0.0)
        d_new = new_g.get("doom_fps", 0.0)
        print(f"{'Doom (demo3) FPS':<30} | {d_old:<15.2f} | {d_new:<15.2f} | {format_delta(d_old, d_new, True)}")

    if "pcpbench_fps" in old_g or "pcpbench_fps" in new_g:
        p_old = old_g.get("pcpbench_fps", 0.0)
        p_new = new_g.get("pcpbench_fps", 0.0)
        print(f"{'PCPBench (vga) FPS':<30} | {p_old:<15.2f} | {p_new:<15.2f} | {format_delta(p_old, p_new, True)}")

    if "dhrystones_per_sec" in old_g or "dhrystones_per_sec" in new_g:
        dh_old = old_g.get("dhrystones_per_sec", 0.0)
        dh_new = new_g.get("dhrystones_per_sec", 0.0)
        print(f"{'Dhrystone Ops/sec':<30} | {dh_old:<15.0f} | {dh_new:<15.0f} | {format_delta(dh_old, dh_new, True)}")

    print("-" * 80)

    # 2. Host Wall Clock
    t_old = old_res.get("wall_clock_seconds", 0.0)
    t_new = new_res.get("wall_clock_seconds", 0.0)
    print(f"{'Host Wall-Clock Time (s)':<30} | {t_old:<15.2f} | {t_new:<15.2f} | {format_delta(t_old, t_new, False)}")

    # 3. Host Perf Stats (if available)
    if "instructions" in old_p or "instructions" in new_p:
        i_old = old_p.get("instructions", 0.0)
        i_new = new_p.get("instructions", 0.0)
        print(f"{'Host Instructions Retired':<30} | {i_old:<15.2e} | {i_new:<15.2e} | {format_delta(i_old, i_new, False)}")

    if "cycles" in old_p or "cycles" in new_p:
        c_old = old_p.get("cycles", 0.0)
        c_new = new_p.get("cycles", 0.0)
        print(f"{'Host CPU Cycles':<30} | {c_old:<15.2e} | {c_new:<15.2e} | {format_delta(c_old, c_new, False)}")

    if "l1_dcache_misses" in old_p or "l1_dcache_misses" in new_p:
        m_old = old_p.get("l1_dcache_misses", 0.0)
        m_new = new_p.get("l1_dcache_misses", 0.0)
        print(f"{'Host L1 Cache Misses':<30} | {m_old:<15.2e} | {m_new:<15.2e} | {format_delta(m_old, m_new, False)}")

    print("=" * 80 + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--old-emu", type=Path, default=Path("build/regular/src/86Box"), help="Path to old dynarec 86Box binary")
    parser.add_argument("--new-emu", type=Path, default=Path("build/new_dynarec_dev_debug/src/86Box"), help="Path to new dynarec 86Box binary")
    parser.add_argument("--vm-dir", type=Path, default=Path("benchmarks/vm"), help="Path to VM directory containing 86box.cfg")
    parser.add_argument("--roms", type=Path, default=None, help="Path to 86Box ROMs directory (optional)")
    parser.add_argument("--disk-image", type=Path, default=None, help="Virtual disk image to inject AUTOEXEC.BAT into (optional)")
    parser.add_argument("--timeout", type=float, default=180.0, help="Per-run timeout in seconds (default: 180s)")
    parser.add_argument("--perf", action="store_true", help="Record hardware performance counters using linux 'perf stat'")
    parser.add_argument("--no-headless", action="store_true", help="Display GUI window instead of running offscreen")
    parser.add_argument("--output-json", type=Path, default=None, help="Save structured results to JSON file")
    parser.add_argument("--only", choices=["old", "new"], default=None, help="Only run old or new dynarec")
    parser.add_argument("--results-file", type=Path, default=None, help="Path to COM1/LPT1 results log file")
    parser.add_argument("--break-before", type=str, default="", help="Comma-separated test names to break before (e.g. quake, pcpbench)")
    parser.add_argument("--break-after", type=str, default="", help="Comma-separated test names to break after (e.g. doom, quake)")
    parser.add_argument("--stop-after", type=str, default="", help="Stop execution immediately after a specific test (e.g. doom)")
    parser.add_argument("--per-test-timeout", type=float, default=60.0, help="Per-test timeout in seconds before triggering a hang breakpoint (default: 60s)")
    parser.add_argument("--interactive", action="store_true", help="Prompt interactive continue/screenshot/quit on breakpoint")
    parser.add_argument("--screenshot-dir", type=Path, default=Path("scratch"), help="Directory to save breakpoint screenshots")

    args = parser.parse_args()
    script_dir = Path(__file__).resolve().parent

    # Breakpoint and filtering configuration
    break_before_list = [x.strip().lower() for x in args.break_before.split(",") if x.strip()]
    break_after_list = [x.strip().lower() for x in args.break_after.split(",") if x.strip()]
    stop_after_target = args.stop_after.strip().lower() if args.stop_after else None

    # Check if disk injection requested
    if args.disk_image:
        inject_guest_files(args.disk_image, script_dir)

    # Validate VM directory
    cfg_file = args.vm_dir / "86box.cfg"
    if not cfg_file.exists():
        sys.exit(f"Error: {cfg_file} not found. Please provide a valid --vm-dir.")

    # Determine results file (written via COM1 redirection or 86box.cfg)
    if args.results_file:
        results_file = args.results_file
    else:
        results_file = args.vm_dir / "benchmark_results.log"
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        try:
            cfg.read(cfg_file)
            for sec in ["File (COM) #1", "File (COM 1)", "File (LPT 1)", "File (LPT) #1"]:
                if cfg.has_section(sec) and cfg.has_option(sec, "path"):
                    p = Path(cfg.get(sec, "path"))
                    results_file = p if p.is_absolute() else (args.vm_dir / p)
                    break
        except Exception:
            pass

    old_res: Dict[str, Any] = {}
    new_res: Dict[str, Any] = {}

    headless = not args.no_headless

    if args.only in (None, "old"):
        if not args.old_emu.exists():
            print(f"{COLOR_YELLOW}Warning: Old dynarec binary {args.old_emu} not found. Skipping old run.{COLOR_RESET}")
        else:
            print(f"\n{COLOR_BOLD}=== Running Old Dynarec (codegen) ==={COLOR_RESET}")
            old_res = run_instance(
                args.old_emu,
                args.vm_dir,
                results_file,
                timeout=args.timeout,
                roms_dir=args.roms,
                use_perf=args.perf,
                headless=headless,
                break_before=break_before_list,
                break_after=break_after_list,
                stop_after=stop_after_target,
                per_test_timeout=args.per_test_timeout,
                interactive=args.interactive,
                screenshot_dir=args.screenshot_dir,
            )

    if args.only in (None, "new"):
        if not args.new_emu.exists():
            # Check alternative preset
            alt = Path("build/new_dynarec/src/86Box")
            if alt.exists():
                args.new_emu = alt
            else:
                sys.exit(f"Error: New dynarec binary {args.new_emu} not found. Run 'cmake --build --preset new_dynarec_dev_debug' first.")

        print(f"\n{COLOR_BOLD}=== Running New Dynarec (codegen_new) ==={COLOR_RESET}")
        new_res = run_instance(
            args.new_emu,
            args.vm_dir,
            results_file,
            timeout=args.timeout,
            roms_dir=args.roms,
            use_perf=args.perf,
            headless=headless,
            break_before=break_before_list,
            break_after=break_after_list,
            stop_after=stop_after_target,
            per_test_timeout=args.per_test_timeout,
            interactive=args.interactive,
            screenshot_dir=args.screenshot_dir,
        )

    # Output comparison if both runs completed
    if old_res and new_res:
        print_comparison_table(old_res, new_res)
    elif old_res:
        print(f"\nOld dynarec run completed in {old_res['wall_clock_seconds']}s. Results: {old_res.get('guest_results')}")
    elif new_res:
        print(f"\nNew dynarec run completed in {new_res['wall_clock_seconds']}s. Results: {new_res.get('guest_results')}")

    if args.output_json:
        data = {"old_dynarec": old_res, "new_dynarec": new_res}
        args.output_json.write_text(json.dumps(data, indent=2))
        print(f"Results saved to {args.output_json}")


if __name__ == "__main__":
    main()
