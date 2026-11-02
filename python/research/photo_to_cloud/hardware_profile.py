"""Hardware-aware, conservative defaults for the photo-to-cloud prototype."""

from __future__ import annotations

import ctypes
import json
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys


GIB = 1024 ** 3
AUTO_THREAD_CAP = 16


def total_physical_memory_bytes() -> int | None:
    """Return installed RAM without adding a mandatory third-party dependency."""
    if os.name == "nt":
        class MemoryStatusEx(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatusEx()
        status.dwLength = ctypes.sizeof(status)
        try:
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullTotalPhys)
        except (AttributeError, OSError):
            return None
        return None

    try:
        if sys.platform.startswith("linux"):
            with open("/proc/meminfo", encoding="ascii") as source:
                for line in source:
                    if line.startswith("MemTotal:"):
                        return int(line.split()[1]) * 1024
        page_size = os.sysconf("SC_PAGE_SIZE")
        pages = os.sysconf("SC_PHYS_PAGES")
        return int(page_size) * int(pages)
    except (OSError, ValueError, TypeError, AttributeError):
        return None


def logical_processor_count() -> int:
    process_count = getattr(os, "process_cpu_count", None)
    value = process_count() if process_count is not None else os.cpu_count()
    return max(1, int(value or 1))


def physical_processor_count(logical_count: int | None = None) -> int:
    """Return physical core count where the operating system exposes it."""
    logical = max(1, int(logical_count if logical_count is not None else logical_processor_count()))
    if os.name == "nt":
        try:
            get_information = ctypes.windll.kernel32.GetLogicalProcessorInformationEx
            required = ctypes.c_ulong(0)
            get_information(0, None, ctypes.byref(required))
            if required.value:
                buffer = ctypes.create_string_buffer(required.value)
                if get_information(0, buffer, ctypes.byref(required)):
                    offset = cores = 0
                    while offset + 8 <= required.value:
                        relationship, size = struct.unpack_from("=II", buffer.raw, offset)
                        if size < 8 or offset + size > required.value:
                            break
                        if relationship == 0:  # RelationProcessorCore
                            cores += 1
                        offset += size
                    if cores:
                        return min(logical, cores)
        except (AttributeError, OSError, ValueError, struct.error):
            pass
    elif sys.platform.startswith("linux"):
        try:
            current_physical = current_core = None
            cores: set[tuple[str, str]] = set()
            with open("/proc/cpuinfo", encoding="ascii", errors="replace") as source:
                for line in source:
                    if not line.strip():
                        if current_physical is not None and current_core is not None:
                            cores.add((current_physical, current_core))
                        current_physical = current_core = None
                    elif line.startswith("physical id"):
                        current_physical = line.split(":", 1)[1].strip()
                    elif line.startswith("core id"):
                        current_core = line.split(":", 1)[1].strip()
            if current_physical is not None and current_core is not None:
                cores.add((current_physical, current_core))
            if cores:
                return min(logical, len(cores))
        except OSError:
            pass
    elif sys.platform == "darwin":
        try:
            result = subprocess.run(["sysctl", "-n", "hw.physicalcpu"], capture_output=True,
                                    text=True, timeout=2, check=True)
            return min(logical, max(1, int(result.stdout.strip())))
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    return logical


def recommended_threads(cpu_count: int | None = None,
                        memory_bytes: int | None = None,
                        physical_count: int | None = None) -> int:
    """Cap parallel jobs by physical cores and roughly 4 GiB RAM per worker."""
    logical = max(1, int(cpu_count if cpu_count is not None else logical_processor_count()))
    physical = max(1, int(physical_count if physical_count is not None else
                          (physical_processor_count(logical) if cpu_count is None else math.ceil(logical / 2))))
    memory = total_physical_memory_bytes() if memory_bytes is None else memory_bytes
    memory_limit = AUTO_THREAD_CAP
    if memory:
        memory_limit = max(1, int(int(memory) / (4 * GIB) + 0.5))
    return max(1, min(AUTO_THREAD_CAP, logical, physical, memory_limit))


def cpu_model_name() -> str:
    if os.name == "nt":
        try:
            import winreg

            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                value, _ = winreg.QueryValueEx(key, "ProcessorNameString")
                if value and str(value).strip():
                    return " ".join(str(value).split())
        except (ImportError, OSError):
            pass
    if sys.platform.startswith("linux"):
        try:
            with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as source:
                for line in source:
                    if line.lower().startswith(("model name", "hardware")):
                        _, value = line.split(":", 1)
                        if value.strip():
                            return " ".join(value.split())
        except OSError:
            pass
    return platform.processor().strip() or platform.machine() or "CPU не определён"


def opencl_gpu_inventory() -> list[dict]:
    """List visible OpenCL GPUs; this only inventories them, it does not certify them."""
    try:
        import pyopencl as cl
        gpu_type = cl.device_type.GPU
        platforms = cl.get_platforms()
    except Exception:
        return []
    found = []
    for platform_index, platform_item in enumerate(platforms):
        try:
            devices = platform_item.get_devices(device_type=gpu_type)
        except Exception:
            continue
        for device_index, device in enumerate(devices):
            if not getattr(device, "available", False):
                continue
            memory = int(getattr(device, "global_mem_size", 0) or 0)
            found.append({
                "name": " ".join(str(getattr(device, "name", "GPU")).split()),
                "vendor": " ".join(str(getattr(device, "vendor", "")).split()),
                "memory_bytes": memory,
                "platform_index": platform_index,
                "device_index": device_index,
            })
    return found


def _default_engine_path(module: str, function: str) -> Path | None:
    try:
        imported = __import__(module, fromlist=[function])
        return Path(getattr(imported, function)())
    except (ImportError, OSError, TypeError):
        return None


def _openmvs_ready(path: Path | None) -> bool:
    if path is None or not path.is_dir():
        return False
    suffix = ".exe" if os.name == "nt" else ""
    return ((path / f"InterfaceCOLMAP{suffix}").is_file()
            and (path / f"DensifyPointCloud{suffix}").is_file())


def _resolve_openmvs(path_text: str) -> tuple[Path | None, bool]:
    if path_text.strip():
        path = Path(path_text).expanduser()
    else:
        path = _default_engine_path("openmvs_install", "default_destination")
        if not _openmvs_ready(path):
            legacy = _default_engine_path("openmvs_install", "legacy_destination")
            if _openmvs_ready(legacy):
                path = legacy
    return path, _openmvs_ready(path)


def _resolve_colmap(path_text: str) -> tuple[Path | None, bool, str | None]:
    executable = Path(path_text).expanduser() if path_text.strip() else _default_engine_path(
        "colmap_cl_install", "default_destination")
    if executable is None:
        return None, False, None
    if executable.is_dir():
        executable = executable / "bin" / ("colmap.exe" if os.name == "nt" else "colmap")
    if not executable.is_file():
        return executable, False, None

    try:
        from colmap_cl_backend import backend_environment, select_target_gpu, verify_backend

        environment = backend_environment(executable)
        verify_backend(executable, environment)
        return executable, True, None
    except Exception as exc:
        message = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
        return executable, False, message[:240]


def run_opencl_probe(verify: bool = True, timeout_seconds: int = 35) -> dict:
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--hardware-opencl-probe"]
    else:
        command = [sys.executable, str(Path(__file__).with_name("app.py")), "--hardware-opencl-probe"]
    if not verify:
        command.append("--inventory-only")
    try:
        result = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", timeout=timeout_seconds,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except subprocess.TimeoutExpired:
        return {"gpus": [], "verified_amd_gpu": None,
                "error": f"проверка OpenCL не завершилась за {timeout_seconds} секунд"}
    except OSError as exc:
        return {"gpus": [], "verified_amd_gpu": None,
                "error": f"не удалось запустить диагностику OpenCL: {exc}"}
    try:
        payload = json.loads(result.stdout.strip().splitlines()[-1])
        if isinstance(payload, dict):
            return payload
    except (IndexError, json.JSONDecodeError):
        pass
    message = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else "нет ответа диагностики"
    return {"gpus": [], "verified_amd_gpu": None,
            "error": message[:240] if result.returncode else "не удалось прочитать ответ диагностики"}


def hardware_opencl_probe_main(verify: bool = True) -> int:
    """Entry point for the bounded OpenCL child process."""
    devices = opencl_gpu_inventory()
    if not verify:
        print(json.dumps({"gpus": devices, "verified_amd_gpu": None, "error": None},
                         ensure_ascii=False))
        return 0
    try:
        from colmap_cl_backend import select_target_gpu

        selected = select_target_gpu()
        target = next((device for device in devices
                       if device["platform_index"] == selected["platform_index"]
                       and device["device_index"] == selected["device_index"]), None)
        if target is None:
            target = {"name": selected["device"], "vendor": "AMD", "memory_bytes": 0,
                      "platform_index": selected["platform_index"],
                      "device_index": selected["device_index"]}
        print(json.dumps({"gpus": devices, "verified_amd_gpu": target, "error": None},
                         ensure_ascii=False))
        return 0
    except Exception as exc:
        message = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
        print(json.dumps({"gpus": devices, "verified_amd_gpu": None,
                          "error": message[:240]}, ensure_ascii=False))
        return 1


def recommend_mode(amd_opencl_verified: bool, colmap_cl_ready: bool,
                   openmvs_ready: bool) -> tuple[str, str]:
    if amd_opencl_verified and colmap_cl_ready:
        return "dense-opencl", "AMD OpenCL и COLMAP-CL прошли проверку"
    if openmvs_ready:
        return "dense-cpu", "плотная обработка доступна через OpenMVS на CPU"
    return "sparse-cpu", "движки плотной обработки не найдены; доступно разреженное облако CPU"


def detect_hardware_profile(openmvs_path: str = "", colmap_path: str = "") -> dict:
    """Inspect local resources and return a safe mode recommendation for the GUI."""
    logical = logical_processor_count()
    physical = physical_processor_count(logical)
    memory = total_physical_memory_bytes()
    openmvs, openmvs_ready = _resolve_openmvs(openmvs_path)
    colmap, colmap_ready, colmap_error = _resolve_colmap(colmap_path)
    # Run device enumeration and kernel verification out of process. A bad
    # driver or a stalled OpenCL compiler cannot freeze the desktop window.
    probe = run_opencl_probe(verify=colmap_ready, timeout_seconds=35 if colmap_ready else 12)
    gpus = probe.get("gpus", [])
    verified_gpu = probe.get("verified_amd_gpu")
    opencl_error = probe.get("error") or colmap_error
    recommended_mode, reason = recommend_mode(
        amd_opencl_verified=verified_gpu is not None,
        colmap_cl_ready=colmap_ready,
        openmvs_ready=openmvs_ready,
    )
    if verified_gpu is not None:
        gpu_names = [verified_gpu["name"]]
    else:
        gpu_names = [entry["name"] for entry in gpus]
    gpu_by_name = {entry["name"]: entry for entry in gpus}
    if verified_gpu is not None:
        gpu_by_name[verified_gpu["name"]] = verified_gpu
    gpu_text = ", ".join(
        name + (f" ({gpu_by_name[name]['memory_bytes'] / GIB:.0f} ГБ видеопамяти)"
                if gpu_by_name.get(name, {}).get("memory_bytes") else "")
        for name in gpu_names
    ) if gpu_names else "OpenCL GPU не обнаружена"
    memory_text = f"{memory / GIB:.0f} ГБ RAM" if memory else "объём RAM не определён"
    mode_text = {
        "dense-opencl": "плотное облако — AMD OpenCL",
        "dense-cpu": "плотное облако — CPU (OpenMVS)",
        "sparse-cpu": "разреженное облако — CPU",
    }[recommended_mode]
    summary = (
        f"CPU: {cpu_model_name()} · {physical} ядер / {logical} потоков · {memory_text}; "
        f"GPU: {gpu_text}.\nАвтоподбор: {mode_text}; {reason}."
    )
    if opencl_error and not verified_gpu:
        summary += f" Проверка AMD/OpenCL: {opencl_error}."
    return {
        "cpu_name": cpu_model_name(),
        "logical_processors": logical,
        "physical_cores": physical,
        "total_memory_bytes": memory,
        "gpus": gpus,
        "verified_amd_opencl_gpu": verified_gpu,
        "openmvs_path": str(openmvs) if openmvs_ready else None,
        "openmvs_ready": openmvs_ready,
        "colmap_path": str(colmap) if colmap_ready else None,
        "colmap_cl_ready": colmap_ready,
        "recommended_threads": recommended_threads(logical, memory, physical),
        "recommended_mode": recommended_mode,
        "reason": reason,
        "summary": summary,
    }
