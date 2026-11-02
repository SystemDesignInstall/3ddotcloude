import unittest
from pathlib import Path
import subprocess
from unittest.mock import patch

import hardware_profile


class HardwareProfileTests(unittest.TestCase):
    def test_threads_use_cpu_and_ram_budgets(self):
        gib = 1024 ** 3
        self.assertEqual(hardware_profile.recommended_threads(32, 64 * gib, 16), 16)
        self.assertEqual(hardware_profile.recommended_threads(32, 32 * gib, 16), 8)
        self.assertEqual(hardware_profile.recommended_threads(16, 64 * gib, 8), 8)
        self.assertEqual(hardware_profile.recommended_threads(8, 64 * gib, 4), 4)
        self.assertEqual(hardware_profile.recommended_threads(32, 2 * gib, 16), 1)

    def test_opencl_is_recommended_only_when_gpu_and_colmap_are_verified(self):
        gpu = {"name": "AMD Radeon RX 7900 XTX", "vendor": "AMD", "memory_bytes": 24 * 1024 ** 3}
        with patch.object(hardware_profile, "logical_processor_count", return_value=32), \
             patch.object(hardware_profile, "physical_processor_count", return_value=16), \
             patch.object(hardware_profile, "total_physical_memory_bytes", return_value=64 * 1024 ** 3), \
             patch.object(hardware_profile, "cpu_model_name", return_value="AMD Ryzen 9 9950X"), \
             patch.object(hardware_profile, "opencl_gpu_inventory", return_value=[gpu]), \
             patch.object(hardware_profile, "_resolve_openmvs", return_value=(None, False)), \
             patch.object(hardware_profile, "_resolve_colmap",
                          return_value=(Path("colmap.exe"), True, None)), \
             patch.object(hardware_profile, "run_opencl_probe",
                          return_value={"gpus": [gpu], "verified_amd_gpu": gpu, "error": None}):
            profile = hardware_profile.detect_hardware_profile()

        self.assertEqual(profile["recommended_mode"], "dense-opencl")
        self.assertEqual(profile["recommended_threads"], 16)
        self.assertEqual(profile["verified_amd_opencl_gpu"]["name"], gpu["name"])
        self.assertIn("AMD OpenCL", profile["summary"])

    def test_detected_gpu_without_a_verified_backend_falls_back_to_available_cpu_engine(self):
        gpu = {"name": "AMD Radeon RX 7900 XTX", "vendor": "AMD", "memory_bytes": 24 * 1024 ** 3}
        openmvs = Path("openmvs")
        with patch.object(hardware_profile, "logical_processor_count", return_value=32), \
             patch.object(hardware_profile, "physical_processor_count", return_value=16), \
             patch.object(hardware_profile, "total_physical_memory_bytes", return_value=64 * 1024 ** 3), \
             patch.object(hardware_profile, "cpu_model_name", return_value="AMD Ryzen 9 9950X"), \
             patch.object(hardware_profile, "opencl_gpu_inventory", return_value=[gpu]), \
             patch.object(hardware_profile, "_resolve_openmvs", return_value=(openmvs, True)), \
             patch.object(hardware_profile, "_resolve_colmap",
                          return_value=(Path("colmap.exe"), False, "COLMAP-CL check failed")), \
             patch.object(hardware_profile, "run_opencl_probe",
                          return_value={"gpus": [gpu], "verified_amd_gpu": None, "error": None}):
            profile = hardware_profile.detect_hardware_profile()

        self.assertEqual(profile["recommended_mode"], "dense-cpu")
        self.assertEqual(profile["openmvs_path"], str(openmvs))
        self.assertIn("плотная обработка", profile["summary"])
        self.assertIn("COLMAP-CL check failed", profile["summary"])

    def test_no_dense_engine_falls_back_to_available_sparse_cpu_mode(self):
        self.assertEqual(
            hardware_profile.recommend_mode(False, False, False),
            ("sparse-cpu", "движки плотной обработки не найдены; доступно разреженное облако CPU"),
        )

    def test_finds_previous_openmvs_folder_when_new_texturing_install_is_absent(self):
        current = Path("new-textured")
        legacy = Path("openmvs-2.4.0")
        with patch.object(hardware_profile, "_default_engine_path",
                          side_effect=lambda module, function: current if function == "default_destination" else legacy), \
             patch.object(hardware_profile, "_openmvs_ready", side_effect=lambda path: path == legacy):
            path, ready = hardware_profile._resolve_openmvs("")
        self.assertEqual(path, legacy)
        self.assertTrue(ready)

    def test_hung_opencl_driver_returns_cpu_fallback_reason(self):
        with patch.object(hardware_profile.subprocess, "run",
                          side_effect=subprocess.TimeoutExpired("probe", timeout=12)):
            profile = hardware_profile.run_opencl_probe(verify=False, timeout_seconds=12)

        self.assertIsNone(profile["verified_amd_gpu"])
        self.assertIn("не завершилась за 12 секунд", profile["error"])


if __name__ == "__main__":
    unittest.main()
