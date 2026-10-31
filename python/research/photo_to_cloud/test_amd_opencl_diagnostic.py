"""Device selection must never report a CPU or the wrong GPU as the target."""

import unittest
from types import SimpleNamespace

from amd_opencl_diagnostic import choose_device


class FakePlatform:
    def __init__(self, devices):
        self.devices = devices

    def get_devices(self, device_type):
        return self.devices


def device(name, kind, vendor=0x1002, available=True):
    return SimpleNamespace(name=name, type=kind, vendor_id=vendor, available=available)


def api(platforms):
    return SimpleNamespace(get_platforms=lambda: platforms,
                           device_type=SimpleNamespace(ALL=0xFFFFFFFF, CPU=2, GPU=4),
                           LogicError=RuntimeError)


class DeviceSelectionTests(unittest.TestCase):
    def test_prefers_the_discrete_7900_xtx_when_an_integrated_gpu_is_present(self):
        cpu = device("CPU", 2, vendor=0)
        integrated = device("AMD Radeon Graphics", 4)
        discrete = device("AMD Radeon RX 7900 XTX", 4)
        selected = choose_device(api([FakePlatform([cpu, integrated, discrete])]))
        self.assertEqual((selected[0], selected[1], selected[3]), (0, 2, discrete))

    def test_cpu_and_other_vendors_are_not_reported_as_amd_gpu(self):
        with self.assertRaisesRegex(RuntimeError, "не найдено"):
            choose_device(api([FakePlatform([device("CPU", 2), device("Other GPU", 4, vendor=1)])]))

    def test_ambiguous_amd_gpus_are_not_silently_selected(self):
        with self.assertRaisesRegex(RuntimeError, "несколько GPU"):
            choose_device(api([FakePlatform([device("Radeon A", 4), device("Radeon B", 4)])]))

    def test_cpu_test_mode_does_not_select_a_gpu(self):
        gpu, cpu = device("AMD GPU", 4), device("CPU", 2, vendor=0)
        selected = choose_device(api([FakePlatform([gpu, cpu])]), allow_cpu=True)
        self.assertEqual((selected[1], selected[3]), (1, cpu))


if __name__ == "__main__":
    unittest.main()
