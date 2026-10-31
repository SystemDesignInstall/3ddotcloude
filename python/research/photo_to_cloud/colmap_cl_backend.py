"""Experimental subprocess adapter for an independently installed COLMAP-CL 1.5."""
from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading

from amd_opencl_diagnostic import find_devices, is_target, verify_buffer_kernel, verify_image_kernel


class BackendError(Exception):
    pass


def select_target_gpu(cl=None, np=None, verify_buffer=verify_buffer_kernel, verify_image=verify_image_kernel):
    """Resolve a current OpenCL index; never silently substitute a CPU or another GPU."""
    if cl is None or np is None:
        try:
            import numpy as np
            import pyopencl as cl
        except ImportError as exc:
            raise BackendError("В сборке отсутствуют компоненты NumPy/PyOpenCL.") from exc
    candidates = [entry for entry in find_devices(cl) if is_target(entry[3])]
    if not candidates:
        raise BackendError("Не найдена доступная GPU AMD с именем gfx1100 или RX 7900 XTX.")
    errors = []
    for platform_id, device_id, platform, device in candidates:
        if device_id >= 1000:
            errors.append(f"{platform_id}/{device_id}: индекс устройства слишком велик")
            continue
        try:
            verify_buffer(cl, np, device)
            verify_image(cl, np, device)
        except Exception as exc:
            summary = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
            errors.append(f"{platform_id}/{device_id}: {summary[:250]}")
            continue
        return {"platform_index": platform_id, "device_index": device_id,
                "colmap_gpu_index": platform_id * 1000 + device_id,
                "platform": platform.name.strip(), "device": device.name.strip(),
                "driver": device.driver_version.strip(), "opencl": device.version.strip()}
    raise BackendError("Ни одна запись gfx1100 не прошла проверку OpenCL: " + "; ".join(errors))


def backend_environment(exe: Path) -> dict[str, str]:
    lib = exe.parent.parent / "lib"
    if not exe.is_file() or not lib.is_dir():
        raise BackendError("Не найдены bin/colmap.exe и соседняя папка lib движка COLMAP-CL.")
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(lib), str(exe.parent), env.get("PATH", "")])
    env["QT_PLUGIN_PATH"] = os.pathsep.join([str(lib), env.get("QT_PLUGIN_PATH", "")])
    return env


@contextmanager
def external_dll_search():
    # PyInstaller changes Windows DLL search rules for its own bundled modules.
    # Restore standard rules while launching an unrelated executable, then put
    # the worker's search path back. This does not modify the system PATH.
    if os.name != "nt" or not getattr(sys, "frozen", False):
        yield
        return
    import ctypes
    kernel = ctypes.windll.kernel32
    if not kernel.SetDllDirectoryW(None):
        raise OSError("Could not reset the DLL search path")
    try:
        yield
    finally:
        kernel.SetDllDirectoryW(str(sys._MEIPASS))


def verify_backend(exe: Path, env: dict[str, str]) -> None:
    try:
        with external_dll_search():
            result = subprocess.run([str(exe), "-h"], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                    timeout=20, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BackendError(f"Не удалось запустить внешний COLMAP-CL: {exc}") from exc
    if result.returncode != 0 or "COLMAP-CL Version 1.5" not in result.stdout:
        raise BackendError("Выбранный файл не определён как COLMAP-CL 1.5.")


def run_program(args: list[str], cwd: Path, env: dict[str, str], log: Path,
                check_cancel, expected_device: dict | None = None) -> None:
    """Stream a child process, support cancellation, and validate its device log."""
    check_cancel()
    try:
        with external_dll_search():
            process = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                       bufsize=1, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as exc:
        raise BackendError(f"Не удалось запустить COLMAP-CL: {exc}") from exc
    events: queue.Queue = queue.Queue()

    def read_output() -> None:
        try:
            assert process.stdout is not None
            for line in process.stdout:
                events.put(line)
        finally:
            events.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    observed_devices, observed_platforms, observed_indices = set(), set(), set()
    try:
        with log.open("a", encoding="utf-8") as output:
            output.write("\nCOMMAND: " + " ".join(args) + "\n")
            while True:
                check_cancel()
                try:
                    line = events.get(timeout=0.1)
                except queue.Empty:
                    continue
                if line is None:
                    break
                clean = line.replace("\x00", "")
                print(clean, end="", flush=True)
                output.write(clean)
                output.flush()
                match = re.search(r"OpenCL device\s*:\s*(.+)", clean, re.IGNORECASE)
                if match:
                    observed_devices.add(match.group(1).strip().casefold())
                match = re.search(r"OpenCL platform\s*:\s*(.+)", clean, re.IGNORECASE)
                if match:
                    observed_platforms.add(match.group(1).strip().casefold())
                match = re.search(r"\bgpu_index\s*:\s*(-?\d+)", clean, re.IGNORECASE)
                if match:
                    observed_indices.add(int(match.group(1)))
        code = process.wait()
        check_cancel()
        if code != 0:
            raise BackendError(f"COLMAP-CL завершился с кодом {code}; подробности в colmap-cl.log.")
        if expected_device is not None:
            if (observed_devices != {expected_device["device"].casefold()}
                    or observed_platforms != {expected_device["platform"].casefold()}
                    or observed_indices != {expected_device["colmap_gpu_index"]}):
                raise BackendError("COLMAP-CL не подтвердил выбранную GPU в журнале. Облако не считается проверенным.")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        reader.join(timeout=2)
        if process.stdout is not None:
            process.stdout.close()
