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


def choose_device(cl, allow_cpu: bool = False):
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
    if not allow_cpu and len(devices) > 1:
        preferred = [entry for entry in devices if "7900 xtx" in entry[3].name.lower()
                     or "gfx1100" in entry[3].name.lower()]
        if len(preferred) == 1:
            devices = preferred
        else:
            names = ", ".join(entry[3].name.strip() for entry in devices)
            raise RuntimeError(f"Обнаружено несколько GPU AMD: {names}. Нужен выбор устройства.")
    return devices[0]


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


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Проверка AMD OpenCL")
    parser.add_argument("--test-cpu", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--no-pause", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    lines = ["Spatial Photo Cloud — проверка OpenCL", "Фотографии не используются, данные в сеть не отправляются."]
    code = 1
    try:
        import numpy as np
        import pyopencl as cl
        platform_id, device_id, platform, device = choose_device(cl, args.test_cpu)
        lines.extend([
            "Режим: тест CPU (только для автоматической проверки)" if args.test_cpu else "Режим: GPU AMD",
            f"Платформа: {platform.name.strip()}",
            f"Устройство: {device.name.strip()}",
            f"Производитель: {device.vendor.strip()}",
            f"Драйвер: {device.driver_version.strip()}",
            f"OpenCL: {device.version.strip()}",
            f"Индекс платформы/устройства: {platform_id}/{device_id}",
        ])
        verify_image_kernel(cl, np, device)
        lines.append("РЕЗУЛЬТАТ: тест изображений и вычисления OpenCL пройден.")
        lines.append("Это тест драйвера, а не полноценная реконструкция фотографий.")
        code = 0
    except Exception as exc:
        lines.append(f"РЕЗУЛЬТАТ: тест не пройден: {exc}")
    report = "\n".join(lines) + "\n"
    print(report, flush=True)
    directory = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path.cwd()
    try:
        path = directory / "AMD_OpenCL_Report.txt"
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
