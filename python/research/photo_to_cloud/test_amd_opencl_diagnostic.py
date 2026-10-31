"""Exercise repeated devices and failures without pretending a CPU is an AMD GPU."""

import unittest
from types import SimpleNamespace
from unittest.mock import patch

from amd_opencl_diagnostic import diagnose_devices, find_devices


class FakePlatform:
    name = "Test OpenCL platform"
    version = "OpenCL 1.2 Test"

    def __init__(self, devices):
        self.devices = devices

    def get_devices(self, device_type):
        return self.devices


def device(name, kind, vendor=0x1002, available=True):
    return SimpleNamespace(name=name, type=kind, vendor_id=vendor, available=available,
                           vendor="AMD", driver_version="test", version="OpenCL 1.2")


def api(platforms):
    return SimpleNamespace(get_platforms=lambda: platforms,
                           device_type=SimpleNamespace(ALL=0xFFFFFFFF, CPU=2, GPU=4),
                           LogicError=RuntimeError)


class DeviceSelectionTests(unittest.TestCase):
    def test_both_platforms_are_checked_even_with_repeated_names(self):
        platforms = [FakePlatform([device("gfx1036", 4), device("gfx1100", 4)]),
                     FakePlatform([device("gfx1036", 4), device("gfx1100", 4)])]
        cl = api(platforms)
        found = find_devices(cl)
        self.assertEqual([(p, d) for p, d, _, _ in found], [(0, 0), (0, 1), (1, 0), (1, 1)])
        with patch("amd_opencl_diagnostic.verify_buffer_kernel") as buffer_test, \
             patch("amd_opencl_diagnostic.verify_image_kernel") as image_test:
            lines, passed = diagnose_devices(cl, None, found)
        self.assertTrue(passed)
        self.assertEqual(buffer_test.call_count, 4)
        self.assertEqual(image_test.call_count, 4)
        self.assertIn("Итого для gfx1100/RX 7900 XTX: успешно 2 из 2 записей.", lines)
        self.assertIn("Платформа/устройство: 1/1", lines)

    def test_other_gpu_success_does_not_hide_target_failure(self):
        found = find_devices(api([FakePlatform([device("gfx1036", 4), device("gfx1100", 4)])]))
        def image_test(cl, np, entry):
            if entry.name == "gfx1100":
                raise RuntimeError("compiler failed\nprivate compiler log")
        with patch("amd_opencl_diagnostic.verify_buffer_kernel"), \
             patch("amd_opencl_diagnostic.verify_image_kernel", side_effect=image_test):
            lines, passed = diagnose_devices(None, None, found)
        self.assertFalse(passed)
        self.assertIn("Итого для gfx1100/RX 7900 XTX: успешно 0 из 1 записей.", lines)
        self.assertTrue(any("compiler failed" in line for line in lines))
        self.assertFalse(any("private compiler log" in line for line in lines))

    def test_partial_success_is_individual_and_does_not_skip_other_entries(self):
        entries = [device("gfx1100", 4), device("gfx1100", 4)]
        found = find_devices(api([FakePlatform(entries)]))
        def image_test(cl, np, entry):
            if entry is entries[0]:
                raise RuntimeError("first platform failed")
        with patch("amd_opencl_diagnostic.verify_buffer_kernel") as buffer_test, \
             patch("amd_opencl_diagnostic.verify_image_kernel", side_effect=image_test):
            lines, passed = diagnose_devices(None, None, found)
        self.assertTrue(passed)
        self.assertEqual(buffer_test.call_count, 2)
        self.assertIn("Итого для gfx1100/RX 7900 XTX: успешно 1 из 2 записей.", lines)

    def test_cpu_and_other_vendors_are_not_reported_as_amd_gpu(self):
        with self.assertRaisesRegex(RuntimeError, "не найдено"):
            find_devices(api([FakePlatform([device("CPU", 2), device("Other GPU", 4, vendor=1)])]))

    def test_cpu_test_mode_does_not_select_a_gpu(self):
        gpu, cpu = device("AMD GPU", 4), device("CPU", 2, vendor=0)
        selected = find_devices(api([FakePlatform([gpu, cpu])]), allow_cpu=True)
        self.assertEqual([(entry[1], entry[3]) for entry in selected], [(1, cpu)])

    def test_unavailable_device_is_ignored(self):
        with self.assertRaisesRegex(RuntimeError, "не найдено"):
            find_devices(api([FakePlatform([device("gfx1100", 4, available=False)])]))

    def test_buffer_failure_does_not_prevent_image_test(self):
        found = find_devices(api([FakePlatform([device("gfx1100", 4)])]))
        with patch("amd_opencl_diagnostic.verify_buffer_kernel", side_effect=RuntimeError("buffer failed")), \
             patch("amd_opencl_diagnostic.verify_image_kernel") as image_test:
            lines, passed = diagnose_devices(None, None, found)
        self.assertFalse(passed)
        image_test.assert_called_once()
        self.assertTrue(any("buffer failed" in line for line in lines))


if __name__ == "__main__":
    unittest.main()
