#!/usr/bin/env python3
"""Small offline OpenCL image-kernel diagnostic, separate from reconstruction."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


KERNEL = r"""
const sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
__kernel void check_image(read_only image2d_t source, write_only image2d_t target) {
    const int2 pos = (int2)(get_global_id(0), get_global_id(1));
    const float4 value = read_imagef(source, sampler, pos);
    write_imagef(target, pos, value * 2.0f + 1.0f);
}
"""


def find_devices(cl, allow_cpu: bool = False):
    devices = []
    for platform_index, platform in enumerate(cl.get_platforms()):
        try:
            platform_devices = platform.get_devices(device_type=cl.device_type.ALL)
        except cl.LogicError:
            continue
        for device_index, device in enumerate(platform_devices):
            if not device.available:
                continue
            if allow_cpu:
                allowed = bool(device.type & cl.device_type.CPU)
            else:
                allowed = bool(device.type & cl.device_type.GPU) and device.vendor_id == 0x1002
            if allowed:
                devices.append((platform_index, device_index, platform, device))
    if not devices:
        raise RuntimeError("Подходящее устройство OpenCL не найдено. Проверьте драйвер AMD.")
    # A driver may expose the same device through more than one platform.
    # They cannot safely be deduplicated using name alone: two physical cards
    # can have the same name. Keep and test every platform/device pair.
    return devices


def is_target(device) -> bool:
    name = device.name.casefold()
    return "7900 xtx" in name or "gfx1100" in name


def error_summary(exc: Exception) -> str:
    # Compiler logs may contain user-profile paths: keep only the first line.
    summary = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
    home = str(Path.home())
    return summary.replace(home, "<профиль>").replace(home.replace("\\", "/"), "<профиль>")[:500]


def verify_image_kernel(cl, np, device) -> None:
    if not device.image_support:
        raise RuntimeError("Устройство OpenCL не поддерживает обработку изображений.")
    if not device.compiler_available:
        raise RuntimeError("Для устройства недоступен компилятор OpenCL.")
    context = cl.Context([device])
    queue = cl.CommandQueue(context, device)
    image_format = cl.ImageFormat(cl.channel_order.RGBA, cl.channel_type.FLOAT)
    source = np.arange(4 * 4 * 4, dtype=np.float32).reshape((4, 4, 4)) / 64.0
    result = np.empty_like(source)
    image_in = cl.create_image(context, cl.mem_flags.READ_ONLY | cl.mem_flags.COPY_HOST_PTR,
                               image_format, shape=(4, 4), hostbuf=source)
    image_out = cl.create_image(context, cl.mem_flags.WRITE_ONLY, image_format, shape=(4, 4))
    program = cl.Program(context, KERNEL).build(options=["-cl-std=CL1.2"])
    program.check_image(queue, (4, 4), None, image_in, image_out)
    cl.enqueue_copy(queue, result, image_out, origin=(0, 0, 0), region=(4, 4, 1), is_blocking=True)
    if not np.allclose(result, source * 2.0 + 1.0, rtol=0.0, atol=1e-6):
        raise RuntimeError("Результат тестового вычисления не совпал с ожидаемым.")


def verify_buffer_kernel(cl, np, device) -> None:
    """Check ordinary buffers independently of OpenCL image support."""
    context = cl.Context([device])
    queue = cl.CommandQueue(context, device)
    source = np.arange(32, dtype=np.float32)
    result = np.empty_like(source)
    source_buffer = cl.Buffer(context, cl.mem_flags.READ_ONLY | cl.mem_flags.COPY_HOST_PTR, hostbuf=source)
    result_buffer = cl.Buffer(context, cl.mem_flags.WRITE_ONLY, result.nbytes)
    program = cl.Program(context, "__kernel void check(__global const float *a, __global float *b) "
                         "{ int i=get_global_id(0); b[i]=a[i]*2.0f+1.0f; }").build()
    program.check(queue, source.shape, None, source_buffer, result_buffer)
    cl.enqueue_copy(queue, result, result_buffer, is_blocking=True)
    if not np.allclose(result, source * 2.0 + 1.0, rtol=0.0, atol=1e-6):
        raise RuntimeError("Результат тестового вычисления не совпал с ожидаемым.")


def diagnose_devices(cl, np, devices, allow_cpu: bool = False, buffer_only: bool = False):
    """Test each OpenCL platform/device entry, even if names are repeated."""
    lines = ["Режим: тест CPU (только для автоматической проверки)" if allow_cpu else "Режим: GPU AMD"]
    target_exists = not allow_cpu and any(is_target(entry[3]) for entry in devices)
    if target_exists:
        lines.append("Найдено имя gfx1100 или RX 7900 XTX; это не уникальный идентификатор карты.")
    elif not allow_cpu:
        lines.append("Имя gfx1100 или RX 7900 XTX не найдено; проверяются все найденные GPU AMD.")
    successes = []
    for platform_id, device_id, platform, device in devices:
        target = is_target(device) if not allow_cpu else False
        lines.extend([
            "",
            f"Платформа/устройство: {platform_id}/{device_id}",
            f"Платформа: {platform.name.strip()}",
            f"Версия платформы: {platform.version.strip()}",
            f"Устройство: {device.name.strip()}",
            f"Производитель: {device.vendor.strip()}",
            f"Драйвер: {device.driver_version.strip()}",
            f"OpenCL: {device.version.strip()}",
            f"Совпадение по имени с gfx1100/RX 7900 XTX: {'да' if target else 'нет'}",
        ])
        passed = True
        tests = [("буфера", verify_buffer_kernel)]
        if not buffer_only:
            tests.append(("изображений и вычисления", verify_image_kernel))
        for label, verify in tests:
            try:
                verify(cl, np, device)
                lines.append(f"РЕЗУЛЬТАТ: тест {label} OpenCL пройден.")
            except Exception as exc:
                passed = False
                lines.append(f"РЕЗУЛЬТАТ: тест не пройден ({label}): {error_summary(exc)}")
        successes.append((target, passed))
    relevant = [passed for target, passed in successes if target or not target_exists]
    lines.append("")
    if target_exists:
        lines.append(f"Итого для gfx1100/RX 7900 XTX: успешно {sum(relevant)} из {len(relevant)} записей.")
    else:
        lines.append(f"Итого: успешно {sum(relevant)} из {len(relevant)} записей.")
    lines.append("Повторяющиеся записи могут относиться к одной или к разным видеокартам.")
    lines.append("Это тест драйвера, а не полноценная реконструкция фотографий.")
    return lines, bool(any(relevant))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Проверка AMD OpenCL")
    parser.add_argument("--test-cpu", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--test-buffer", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--no-pause", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.test_buffer and not args.test_cpu:
        parser.error("--test-buffer is only available together with --test-cpu")
    lines = ["Spatial Photo Cloud — проверка OpenCL v2", "Фотографии не используются, данные в сеть не отправляются."]
    code = 1
    try:
        import numpy as np
        import pyopencl as cl
        devices = find_devices(cl, args.test_cpu)
        device_lines, passed = diagnose_devices(cl, np, devices, args.test_cpu, args.test_buffer)
        lines.extend(device_lines)
        code = 0 if passed else 1
    except Exception as exc:
        lines.append(f"РЕЗУЛЬТАТ: тест не пройден: {error_summary(exc)}")
    report = "\n".join(lines) + "\n"
    print(report, flush=True)
    directory = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path.cwd()
    try:
        path = directory / "AMD_OpenCL_Report_v2.txt"
        path.write_text(report, encoding="utf-8")
        print(f"Отчёт сохранён рядом с программой: {path.name}", flush=True)
    except OSError as exc:
        print(f"Не удалось сохранить отчёт; можно сфотографировать окно: {exc}", flush=True)
    if not args.no_pause:
        try:
            input("Нажмите Enter, чтобы закрыть окно...")
        except EOFError:
            pass
    return code


if __name__ == "__main__":
    raise SystemExit(main())
