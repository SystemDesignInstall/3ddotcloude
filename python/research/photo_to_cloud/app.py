#!/usr/bin/env python3
"""Small desktop launcher for the experimental local reconstruction workflow."""

from __future__ import annotations

import datetime
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import uuid

import tkinter as tk
from tkinter import filedialog, font, messagebox, scrolledtext, ttk


MODES = {
    "Плотное облако — CPU (OpenMVS)": "dense-cpu",
    "Разреженное облако — CPU": "sparse-cpu",
    "Плотное облако — AMD OpenCL (эксперимент)": "dense-opencl",
    "Плотное облако — AMD HIP (эксперимент)": "dense-amd",
}
MODE_LABELS = {value: key for key, value in MODES.items()}


def worker_command(images: str, output: Path, mode: str, cancel_file: Path,
                   threads: int | None = None) -> list[str]:
    # Keep the GUI and the command-line worker on the same automatic default.
    # reconstruct.py only imports the standard library at module load time, so
    # importing this helper does not initialize any heavy reconstruction backend.
    from reconstruct import recommended_threads

    selected_threads = recommended_threads() if threads is None else threads
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--worker"]
    else:
        command = [sys.executable, "-u", str(Path(__file__).with_name("reconstruct.py"))]
    return command + ["--images", images, "--output", str(output), "--mode", mode,
                      "--threads", str(selected_threads), "--cancel-file", str(cancel_file)]


def mesh_worker_command(input_path: Path, output_path: Path, cancel_file: Path,
                        threads: int) -> list[str]:
    """Run Poisson meshing in an isolated process so the window stays responsive."""
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--mesh-worker"]
    else:
        command = [sys.executable, "-u", str(Path(__file__).with_name("mesh_worker.py"))]
    return command + ["--input", str(input_path), "--output", str(output_path),
                      "--threads", str(threads), "--cancel-file", str(cancel_file)]


def texture_worker_command(mesh_path: Path, workspace: Path, openmvs_dir: Path,
                           output_path: Path, cancel_file: Path,
                           threads: int) -> list[str]:
    """Run photo texturing in an isolated process so the window stays responsive."""
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--texture-worker"]
    else:
        command = [sys.executable, "-u", str(Path(__file__).with_name("texture_worker.py"))]
    return command + ["--mesh", str(mesh_path), "--workspace", str(workspace),
                      "--openmvs-dir", str(openmvs_dir), "--output", str(output_path),
                      "--threads", str(threads), "--cancel-file", str(cancel_file)]


def a2_worker_command(options: dict, output: Path, cancel_file: Path) -> list[str]:
    command = ([sys.executable, "--a2-import-worker"] if getattr(sys, "frozen", False)
               else [sys.executable, "-u", str(Path(__file__).with_name("a2_import.py"))])
    command += ["--input", options["source"], "--output", str(output),
                "--coordinate-frame", options["coordinate_frame"], "--source-version", options["source_version"],
                "--rgb-encoding", options["rgb_encoding"], "--up-axis", options["up_axis"],
                "--cancel-file", str(cancel_file)]
    if options["controls"]:
        command += ["--controls", options["controls"]]
    return command


class App:
    def __init__(self, root: tk.Tk, auto_configure_on_start: bool = True) -> None:
        self.root = root
        root.title("Spatial Photo Cloud")
        root.geometry("820x840")
        root.minsize(700, 680)
        self.events: queue.Queue = queue.Queue()
        self.process: subprocess.Popen | None = None
        self.current_task: str | None = None
        self.mesh_output: Path | None = None
        self.mesh_job_id: str | None = None
        self.texture_output: Path | None = None
        self.cancel_requested = False
        self.cancel_dir: tempfile.TemporaryDirectory | None = None
        self.cancel_file: Path | None = None
        self.installing = False
        self.close_after_cancel = False
        self.result_dir: Path | None = None
        self.viewer_hosts = []
        self.detecting_hardware = False
        self.opening_capture = False
        self.images = tk.StringVar()
        self.destination = tk.StringVar()
        self.openmvs = tk.StringVar()
        self.colmap = tk.StringVar()
        self.mode = tk.StringVar(value=next(iter(MODES)))
        from reconstruct import MAX_THREADS, recommended_threads
        self.threads = tk.StringVar(value=str(recommended_threads()))
        self.status = tk.StringVar(value="Выберите фотографии и папку результата")
        self.hardware_status = tk.StringVar(value="Автоподбор комплектующих ещё не выполнен")

        # Minimal Linux desktops may default to a Latin-only font. Prefer a
        # Unicode font when it is available; Windows keeps its system font.
        if os.name != "nt":
            families = {name.casefold(): name for name in font.families()}
            if "dejavu sans" in families:
                for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
                    font.nametofont(name).configure(family=families["dejavu sans"])
        title_family = font.nametofont("TkDefaultFont").actual()["family"]

        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Фотографии → 3D-модель", font=(title_family, 17, "bold")).pack(anchor="w")
        ttk.Label(frame, text="Обработка на этом компьютере. Результат: облако, сетка и текстурированный GLB.").pack(anchor="w", pady=(4, 12))
        self.path_row(frame, "Папка с фотографиями (CR2, JPG, PNG, TIFF)", self.images, self.choose_images)
        self.path_row(frame, "Где сохранять результат", self.destination, self.choose_destination)
        self.start_button = ttk.Button(frame, text="Построить 3D-модель из фото", command=lambda: self.start(model=True),
                                       state="disabled")
        self.start_button.pack(anchor="w", pady=(8, 0))
        ttk.Label(frame, text="Режим отдельного построения облака").pack(anchor="w", pady=(8, 2))
        self.mode_combo = ttk.Combobox(frame, state="readonly", values=list(MODES), textvariable=self.mode)
        self.mode_combo.pack(fill="x")
        thread_row = ttk.Frame(frame)
        thread_row.pack(fill="x", pady=(8, 0))
        ttk.Label(thread_row, text="Потоки CPU").pack(side="left")
        self.thread_spin = ttk.Spinbox(thread_row, from_=1, to=MAX_THREADS, width=6,
                                       textvariable=self.threads)
        self.thread_spin.pack(side="left", padx=(8, 8))
        ttk.Label(thread_row, text=f"Можно изменить вручную (1–{MAX_THREADS})").pack(side="left")
        hardware_row = ttk.Frame(frame)
        hardware_row.pack(fill="x", pady=(7, 0))
        self.hardware_label = ttk.Label(hardware_row, textvariable=self.hardware_status,
                                        justify="left", wraplength=700)
        self.hardware_label.pack(side="left", fill="x", expand=True)
        self.auto_button = ttk.Button(hardware_row, text="Подобрать автоматически",
                                      command=self.auto_configure)
        self.auto_button.pack(side="right", padx=(8, 0))
        self.path_row(frame, "Папка OpenMVS (плотное облако и текстурирование)", self.openmvs, self.choose_openmvs)
        install_row = ttk.Frame(frame)
        install_row.pack(fill="x", pady=(3, 0))
        self.install_button = ttk.Button(install_row, text="Установить / обновить OpenMVS (22 МБ)", command=self.install_openmvs)
        self.install_button.pack(side="left")
        if os.name != "nt":
            self.install_button.configure(state="disabled")
        ttk.Label(install_row, text="OpenMVS: AGPL-3.0; загружается отдельно").pack(side="left", padx=8)
        self.path_row(frame, "COLMAP-CL для OpenCL / отдельная сборка COLMAP с HIP", self.colmap, self.choose_colmap)
        cl_row = ttk.Frame(frame)
        cl_row.pack(fill="x", pady=(3, 0))
        self.install_cl_button = ttk.Button(cl_row, text="Установить COLMAP-CL для AMD", command=self.install_colmap_cl)
        self.install_cl_button.pack(side="left")
        if os.name != "nt":
            self.install_cl_button.configure(state="disabled")
        ttk.Label(cl_row, text="Внешний экспериментальный движок; скачивается отдельно").pack(side="left", padx=8)
        ttk.Label(frame, text="Масштаб результата по одним фотографиям неизвестен.").pack(anchor="w", pady=(10, 4))

        actions = ttk.Frame(frame)
        actions.pack(fill="x", pady=(0, 8))
        ttk.Button(actions, text="Построить только облако", command=self.start).pack(side="left")
        self.open_button = ttk.Button(actions, text="Открыть папку результата", command=self.open_result, state="disabled")
        self.open_button.pack(side="left", padx=8)
        self.cancel_button = ttk.Button(actions, text="Остановить", command=self.cancel, state="disabled")
        self.cancel_button.pack(side="left", padx=8)
        viewer_actions = ttk.Frame(frame)
        viewer_actions.pack(fill="x", pady=(0, 8))
        self.preview_button = ttk.Button(viewer_actions, text="Открыть построенное облако", command=self.open_preview, state="disabled")
        self.preview_button.pack(side="left")
        ttk.Button(viewer_actions, text="Открыть готовый PLY…", command=self.choose_cloud).pack(side="left", padx=8)
        self.mesh_button = ttk.Button(viewer_actions, text="Построить 3D-сетку из PLY…", command=self.build_mesh)
        self.mesh_button.pack(side="left", padx=8)
        texture_actions = ttk.Frame(frame)
        texture_actions.pack(fill="x", pady=(0, 8))
        self.texture_button = ttk.Button(texture_actions, text="Экспортировать текстурированную модель GLB…",
                                         command=self.texture_mesh)
        self.texture_button.pack(side="left")
        capture_actions = ttk.Frame(frame)
        capture_actions.pack(fill="x", pady=(0, 8))
        self.a2_button = ttk.Button(capture_actions, text="Импорт съёмки A2 mapper…", command=self.import_a2)
        self.a2_button.pack(side="left")
        self.a2_report_button = ttk.Button(capture_actions, text="Отчёт съёмки A2", command=self.open_a2_report)
        self.a2_report_button.pack(side="left", padx=8)
        ttk.Button(capture_actions, text="Открыть съёмку…", command=self.choose_capture).pack(side="left")
        ttk.Label(frame, textvariable=self.status).pack(anchor="w", pady=(0, 4))
        self.progress = ttk.Progressbar(frame, mode="indeterminate")
        self.progress.pack(fill="x", pady=(0, 8))
        self.log = scrolledtext.ScrolledText(frame, height=11, state="disabled", wrap="word")
        self.log.pack(fill="both", expand=True)
        root.protocol("WM_DELETE_WINDOW", self.close)
        if auto_configure_on_start:
            root.after(100, self.auto_configure)
        root.after(100, self.read_events)

    @staticmethod
    def path_row(parent: ttk.Frame, label: str, variable: tk.StringVar, command) -> None:
        ttk.Label(parent, text=label).pack(anchor="w", pady=(7, 2))
        row = ttk.Frame(parent)
        row.pack(fill="x")
        ttk.Entry(row, textvariable=variable).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Выбрать…", command=command).pack(side="left", padx=(8, 0))

    def choose_images(self) -> None:
        value = filedialog.askdirectory(title="Папка с фотографиями")
        if value:
            self.images.set(value)
            if not self.destination.get().strip():
                self.destination.set(str(Path(value).parent / "SpatialPhotoCloud Results"))

    def choose_destination(self) -> None:
        value = filedialog.askdirectory(title="Папка для результатов")
        if value:
            self.destination.set(value)

    def choose_openmvs(self) -> None:
        value = filedialog.askdirectory(title="Папка с InterfaceCOLMAP, DensifyPointCloud и TextureMesh")
        if value:
            self.openmvs.set(value)

    def choose_colmap(self) -> None:
        value = filedialog.askopenfilename(title="Исполняемый файл colmap.exe", filetypes=[("Программа", "*.exe"), ("Все файлы", "*")])
        if value:
            self.colmap.set(value)

    def install_openmvs(self) -> None:
        if self.installing or self.process or self.detecting_hardware:
            return
        self.installing = True
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.start_button.configure(state="disabled")
        self.status.set("Установка OpenMVS…")
        self.progress.start(15)
        def install() -> None:
            try:
                from openmvs_install import default_destination, download_and_install
                path = download_and_install(default_destination(), lambda text: self.events.put(("line", text + "\n")))
                self.events.put(("install_done", str(path)))
            except Exception as exc:
                self.events.put(("install_error", str(exc)))
        threading.Thread(target=install, daemon=True).start()

    def install_colmap_cl(self) -> None:
        if self.installing or self.process or self.detecting_hardware:
            return
        self.installing = True
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.start_button.configure(state="disabled")
        self.status.set("Установка COLMAP-CL…")
        self.progress.start(15)
        def install() -> None:
            try:
                from colmap_cl_install import default_destination, download_and_install
                path = download_and_install(default_destination(), lambda text: self.events.put(("line", text + "\n")))
                self.events.put(("install_cl_done", str(path / "bin" / "colmap.exe")))
            except Exception as exc:
                self.events.put(("install_cl_error", str(exc)))
        threading.Thread(target=install, daemon=True).start()

    def auto_configure(self) -> None:
        """Inspect hardware and installed engines off the UI thread, then apply safe defaults."""
        if self.detecting_hardware or self.installing or self.process:
            return
        self.detecting_hardware = True
        self.start_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.mode_combo.configure(state="disabled")
        self.thread_spin.configure(state="disabled")
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.hardware_status.set("Проверяем CPU, память, видеокарту и доступные движки…")
        self.status.set("Автоматический подбор настроек…")
        self.progress.start(15)
        openmvs_path = self.openmvs.get()
        colmap_path = self.colmap.get()

        def detect() -> None:
            try:
                from hardware_profile import detect_hardware_profile
                profile = detect_hardware_profile(openmvs_path, colmap_path)
                self.events.put(("auto_config_done", profile))
            except Exception as exc:
                self.events.put(("auto_config_error", str(exc)))

        threading.Thread(target=detect, daemon=True).start()

    def finish_auto_configuration(self, profile: dict) -> None:
        from reconstruct import recommended_threads

        self.detecting_hardware = False
        self.progress.stop()
        self.mode_combo.configure(state="readonly")
        self.thread_spin.configure(state="normal")
        self.auto_button.configure(state="normal")
        self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
        self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
        if profile.get("openmvs_path") and not self.openmvs.get().strip():
            self.openmvs.set(profile["openmvs_path"])
        if profile.get("colmap_path") and not self.colmap.get().strip():
            self.colmap.set(profile["colmap_path"])
        mode = profile.get("recommended_mode", "sparse-cpu")
        self.mode.set(MODE_LABELS.get(mode, MODE_LABELS["sparse-cpu"]))
        self.threads.set(str(profile.get("recommended_threads", recommended_threads())))
        self.hardware_status.set(profile.get("summary", "Автонастройка завершена."))
        self.status.set("Настройки подобраны; режим и потоки можно изменить вручную")
        self.start_button.configure(state="normal")
        self.append_log("\nАвтонастройка оборудования завершена.\n")

    def fail_auto_configuration(self, error: str) -> None:
        from reconstruct import recommended_threads

        self.detecting_hardware = False
        self.progress.stop()
        self.mode_combo.configure(state="readonly")
        self.thread_spin.configure(state="normal")
        self.auto_button.configure(state="normal")
        self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
        self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
        self.mode.set(MODE_LABELS["sparse-cpu"])
        self.threads.set(str(recommended_threads()))
        self.hardware_status.set("Не удалось завершить проверку оборудования: " + error)
        self.status.set("Выбран безопасный CPU-режим; проверьте сообщение автоподбора")
        self.start_button.configure(state="normal")

    def append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self, *, model: bool = False) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        if not self.images.get().strip() or not self.destination.get().strip():
            messagebox.showerror("Не выбраны папки", "Выберите папку с фотографиями и папку результата.")
            return
        mode = "dense-cpu" if model else MODES[self.mode.get()]
        openmvs = self.openmvs.get().strip()
        if mode == "dense-cpu" and not openmvs and (not model or os.name != "nt"):
            messagebox.showerror("Нужен OpenMVS", "Выберите папку с программами OpenMVS.")
            return
        if mode in {"dense-amd", "dense-opencl"} and not self.colmap.get().strip():
            messagebox.showerror("Нужен движок COLMAP", "Установите или выберите подходящий colmap.exe.")
            return
        try:
            threads = int(self.threads.get().strip())
        except ValueError:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return
        from reconstruct import MAX_THREADS
        if not 1 <= threads <= MAX_THREADS:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return
        label = datetime.datetime.now().strftime("photo-cloud-%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8]
        self.result_dir = Path(self.destination.get()).expanduser().resolve() / label
        self.cancel_dir = tempfile.TemporaryDirectory(prefix="spatial-cancel-")
        self.cancel_file = Path(self.cancel_dir.name) / "cancel"
        self.close_after_cancel = False
        self.current_task = "model" if model else "reconstruction"
        self.cancel_requested = False
        if model:
            command = ([sys.executable, "--model-worker"] if getattr(sys, "frozen", False)
                       else [sys.executable, "-u", str(Path(__file__).with_name("model_worker.py"))])
            command += ["--images", self.images.get(), "--output", str(self.result_dir),
                        "--threads", str(threads), "--cancel-file", str(self.cancel_file)]
        else:
            command = worker_command(self.images.get(), self.result_dir, mode, self.cancel_file, threads)
        if mode == "dense-cpu" and openmvs:
            command += ["--openmvs-dir", openmvs]
        elif mode in {"dense-amd", "dense-opencl"}:
            command += ["--colmap", self.colmap.get()]
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        environment["PYTHONUNBUFFERED"] = "1"
        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, encoding="utf-8", errors="replace", env=environment,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            self.cancel_dir.cleanup()
            self.cancel_dir = None
            self.cancel_file = None
            self.current_task = None
            messagebox.showerror("Не удалось запустить", str(exc))
            return
        self.append_log(f"\nПапка результата: {self.result_dir}\n")
        self.append_log(f"Потоки CPU: {threads}\n")
        self.start_button.configure(state="disabled")
        self.open_button.configure(state="disabled")
        self.preview_button.configure(state="disabled")
        self.mesh_button.configure(state="disabled")
        self.texture_button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.status.set(f"Обработка… CPU-потоки: {threads}")
        self.progress.start(15)
        threading.Thread(target=self.consume, args=(self.process,), daemon=True).start()

    def build_mesh(self) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        input_name = filedialog.askopenfilename(
            title="Выберите плотное облако PLY",
            filetypes=[("Облако точек PLY", "*.ply"), ("Все файлы", "*")],
        )
        if not input_name:
            return
        input_path = Path(input_name).expanduser().resolve()
        output_name = filedialog.asksaveasfilename(
            title="Сохранить 3D-сетку отдельно от облака",
            initialdir=str(input_path.parent),
            initialfile=f"{input_path.stem}-mesh.ply",
            defaultextension=".ply",
            filetypes=[("Треугольная сетка PLY", "*.ply")],
        )
        if not output_name:
            return
        output_path = Path(output_name).expanduser().resolve()
        if output_path == input_path:
            messagebox.showerror("Нужно отдельное имя", "Исходное облако останется без изменений. Сохраните сетку в отдельный PLY.")
            return
        if output_path.exists() or output_path.with_name(output_path.name + ".report.json").exists():
            messagebox.showerror("Файл уже существует", "Существующие файлы результата не заменяются. Выберите новое имя для сетки.")
            return
        try:
            threads = int(self.threads.get().strip())
        except ValueError:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return
        from reconstruct import MAX_THREADS
        if not 1 <= threads <= MAX_THREADS:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return

        self.cancel_dir = tempfile.TemporaryDirectory(prefix="spatial-mesh-cancel-")
        self.cancel_file = Path(self.cancel_dir.name) / "cancel"
        self.mesh_job_id = Path(self.cancel_dir.name).name
        self.current_task = "mesh"
        self.mesh_output = output_path
        self.cancel_requested = False
        self.close_after_cancel = False
        command = mesh_worker_command(input_path, output_path, self.cancel_file, threads)
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, encoding="utf-8", errors="replace", env=environment,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            self.cancel_dir.cleanup()
            self.cancel_dir = None
            self.cancel_file = None
            self.current_task = None
            self.mesh_output = None
            self.mesh_job_id = None
            messagebox.showerror("Не удалось запустить построение сетки", str(exc))
            return
        self.append_log(f"\nИсходное облако: {input_path}\nРезультат сетки: {output_path}\n")
        self.append_log(f"Потоки CPU: {threads}; этап Poisson выполняется на CPU.\n")
        self.start_button.configure(state="disabled")
        self.open_button.configure(state="disabled")
        self.preview_button.configure(state="disabled")
        self.mesh_button.configure(state="disabled")
        self.texture_button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.status.set("Построение треугольной поверхности из PLY…")
        self.progress.start(15)
        threading.Thread(target=self.consume, args=(self.process,), daemon=True).start()

    def texture_mesh(self) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        if not self.openmvs.get().strip():
            messagebox.showerror("Нужен OpenMVS", "Сначала установите OpenMVS 2.4.0 кнопкой выше.")
            return
        openmvs_dir = Path(self.openmvs.get()).expanduser()
        texturer_name = "TextureMesh.exe" if os.name == "nt" else "TextureMesh"
        if not (openmvs_dir / texturer_name).is_file():
            messagebox.showerror(
                "Нужно обновить OpenMVS",
                "В выбранной папке нет TextureMesh. Нажмите «Установить / обновить OpenMVS»: "
                "новая проверенная версия установится рядом, не заменяя старую."
            )
            return
        mesh_name = filedialog.askopenfilename(
            title="Выберите сетку PLY для наложения фотографий",
            filetypes=[("3D-сетка PLY", "*.ply"), ("Все файлы", "*")],
        )
        if not mesh_name:
            return
        mesh_path = Path(mesh_name).expanduser().resolve()
        initial_workspace = self.result_dir if self.result_dir and self.result_dir.is_dir() else mesh_path.parent
        workspace_name = filedialog.askdirectory(
            title="Выберите dense_workspace или папку результата той же реконструкции",
            initialdir=str(initial_workspace),
        )
        if not workspace_name:
            return
        workspace_path = Path(workspace_name).expanduser().resolve()
        output_name = filedialog.asksaveasfilename(
            title="Сохранить автономную текстурированную модель",
            initialdir=str(mesh_path.parent),
            initialfile=f"{mesh_path.stem}-textured.glb",
            defaultextension=".glb",
            filetypes=[("Текстурированная модель GLB", "*.glb")],
        )
        if not output_name:
            return
        output_path = Path(output_name).expanduser().resolve()
        if output_path == mesh_path:
            messagebox.showerror("Нужно отдельное имя", "Исходная сетка PLY не изменяется. Выберите отдельное имя GLB.")
            return
        if output_path.exists():
            messagebox.showerror("Файл уже существует", "Существующий GLB не заменяется. Выберите другое имя.")
            return
        try:
            threads = int(self.threads.get().strip())
        except ValueError:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return
        from reconstruct import MAX_THREADS
        if not 1 <= threads <= MAX_THREADS:
            messagebox.showerror("Некорректное число потоков", "Введите целое число от 1 до 64.")
            return

        self.cancel_dir = tempfile.TemporaryDirectory(prefix="spatial-texture-cancel-")
        self.cancel_file = Path(self.cancel_dir.name) / "cancel"
        self.current_task = "texture"
        self.texture_output = output_path
        self.cancel_requested = False
        self.close_after_cancel = False
        command = texture_worker_command(mesh_path, workspace_path, openmvs_dir,
                                         output_path, self.cancel_file, threads)
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, encoding="utf-8", errors="replace", env=environment,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            self.cancel_dir.cleanup()
            self.cancel_dir = None
            self.cancel_file = None
            self.current_task = None
            self.texture_output = None
            messagebox.showerror("Не удалось запустить текстурирование", str(exc))
            return
        self.append_log(f"\nСетка: {mesh_path}\nПапка реконструкции: {workspace_path}\nРезультат GLB: {output_path}\n")
        self.append_log(f"OpenMVS TextureMesh; запрошено CPU-потоков: {threads}.\n")
        self.start_button.configure(state="disabled")
        self.open_button.configure(state="disabled")
        self.preview_button.configure(state="disabled")
        self.mesh_button.configure(state="disabled")
        self.texture_button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.auto_button.configure(state="disabled")
        self.status.set("Создание текстурированной модели GLB…")
        self.progress.start(15)
        threading.Thread(target=self.consume, args=(self.process,), daemon=True).start()

    def import_a2(self) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        from a2_import_dialog import show_dialog
        show_dialog(self.root, self.destination.get(), self.start_a2_import)

    def start_a2_import(self, options: dict) -> bool:
        if self.process or self.installing or self.detecting_hardware:
            return False
        output = Path(options["destination"]) / ("a2-capture-" + uuid.uuid4().hex[:12])
        self.cancel_dir = tempfile.TemporaryDirectory(prefix="spatial-a2-cancel-")
        self.cancel_file = Path(self.cancel_dir.name) / "cancel"
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        try:
            self.process = subprocess.Popen(a2_worker_command(options, output, self.cancel_file),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                env=environment, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            self.cancel_dir.cleanup()
            self.cancel_dir = self.cancel_file = None
            messagebox.showerror("Импорт не запущен", str(exc))
            return False
        self.result_dir = output
        self.current_task = "a2"
        self.cancel_requested = self.close_after_cancel = False
        self.destination.set(options["destination"])
        for button in (self.start_button, self.mesh_button, self.texture_button, self.a2_button,
                       self.install_button, self.install_cl_button, self.auto_button,
                       self.preview_button, self.open_button):
            button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self.status.set("Импорт съёмки A2…")
        self.append_log(f"\nИмпорт съёмки A2. Результат: {output}\n")
        self.progress.start(15)
        threading.Thread(target=self.consume, args=(self.process,), daemon=True).start()
        return True

    def open_a2_report(self) -> None:
        import webbrowser
        report = self.result_dir / "capture" / "report.html" if self.result_dir else None
        if report is not None and report.is_file() and (report.parent / "manifest.json").is_file():
            webbrowser.open(report.resolve().as_uri())
        else:
            messagebox.showinfo("Отчёт съёмки", "Сначала импортируйте папку результатов A2 mapper.")

    def consume(self, process: subprocess.Popen) -> None:
        assert process.stdout is not None
        for line in process.stdout:
            self.events.put(("line", line))
        self.events.put(("done", process.wait()))

    def choose_capture(self) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        folder = filedialog.askdirectory(title="Папка сохранённой съёмки A2")
        if folder:
            self.open_capture(Path(folder))

    def open_capture(self, folder: Path) -> None:
        if self.process or self.installing or self.detecting_hardware:
            return
        self.opening_capture = self.installing = True
        self.status.set("Проверка сохранённой съёмки и контрольных сумм…")
        self.progress.start(15)
        def prepare():
            try:
                from capture_session import open_capture
                cloud, capture = open_capture(folder)
                self.events.put(("capture_ready", (cloud, capture)))
            except Exception as exc:
                self.events.put(("capture_error", str(exc)))
        threading.Thread(target=prepare, daemon=True).start()

    def read_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "line":
                    self.append_log(value)
                    if self.current_task == "model" and value.startswith("Этап ") and not self.cancel_requested:
                        self.status.set(value.strip())
                elif kind in ("capture_ready", "capture_error"):
                    self.opening_capture = self.installing = False
                    self.progress.stop()
                    if kind == "capture_ready":
                        cloud, capture = value
                        self.result_dir = cloud.parent
                        self.open_button.configure(state="normal")
                        self.preview_button.configure(state="normal")
                        self.status.set("Съёмка открыта для совместного просмотра")
                        self.open_cloud(cloud, capture=capture)
                    else:
                        self.status.set("Не удалось открыть съёмку")
                        messagebox.showerror("Съёмка не открыта", value)
                elif kind == "auto_config_done":
                    self.finish_auto_configuration(value)
                elif kind == "auto_config_error":
                    self.fail_auto_configuration(value)
                elif kind in {"install_done", "install_error", "install_cl_done", "install_cl_error"}:
                    self.installing = False
                    self.progress.stop()
                    self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.auto_button.configure(state="normal")
                    self.start_button.configure(state="normal")
                    if kind == "install_done":
                        self.openmvs.set(value)
                        self.status.set("OpenMVS установлен")
                        self.append_log(f"OpenMVS: {value}\n")
                    elif kind == "install_cl_done":
                        self.colmap.set(value)
                        self.status.set("COLMAP-CL установлен")
                        self.append_log(f"COLMAP-CL: {value}\n")
                    elif kind == "install_cl_error":
                        self.status.set("Ошибка установки COLMAP-CL")
                        messagebox.showerror("COLMAP-CL", value)
                    else:
                        self.status.set("Ошибка установки OpenMVS")
                        messagebox.showerror("OpenMVS", value)
                    if kind in {"install_done", "install_cl_done"}:
                        self.root.after(100, self.auto_configure)
                else:
                    task = self.current_task
                    mesh_output = self.mesh_output
                    mesh_job_id = self.mesh_job_id
                    texture_output = self.texture_output
                    self.progress.stop()
                    self.process = None
                    if task == "mesh" and mesh_output is not None and mesh_job_id is not None:
                        partials = (
                            mesh_output.with_name(f".{mesh_output.stem}.{mesh_job_id}.partial.ply"),
                            mesh_output.with_name(f".{mesh_output.name}.report.json.{mesh_job_id}.partial"),
                        )
                        for partial in partials:
                            try:
                                partial.unlink(missing_ok=True)
                            except OSError as exc:
                                self.append_log(f"Не удалось удалить временный файл {partial}: {exc}\n")
                    if self.cancel_dir is not None:
                        self.cancel_dir.cleanup()
                    self.cancel_dir = None
                    self.cancel_file = None
                    self.start_button.configure(state="normal")
                    self.cancel_button.configure(state="disabled")
                    self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.auto_button.configure(state="normal")
                    self.mesh_button.configure(state="normal")
                    self.texture_button.configure(state="normal")
                    self.a2_button.configure(state="normal")
                    if self.result_dir and self.result_dir.exists():
                        self.open_button.configure(state="normal")
                        if self.result_cloud() is not None and value == 0:
                            self.preview_button.configure(state="normal")
                    self.current_task = None
                    self.mesh_output = None
                    self.mesh_job_id = None
                    if task == "model" and value == 0 and self.result_dir is not None:
                        model_path = self.result_dir / "model.glb"
                        self.status.set("3D-модель готова. Текстурированный GLB сохранён в папке результата.")
                        self.append_log(f"3D-модель готова: {model_path}\n")
                        self.open_result()
                        self.open_cloud(self.result_dir / "model-mesh.ply")
                    elif task == "mesh":
                        if value == 0 and mesh_output is not None and mesh_output.is_file():
                            self.status.set("3D-сетка построена; открываю её в просмотрщике")
                            self.append_log(f"3D-сетка готова: {mesh_output}\n")
                            self.open_cloud(mesh_output)
                        elif value == 130 or self.cancel_requested:
                            self.status.set("Построение 3D-сетки остановлено; исходное облако не изменено")
                        else:
                            self.status.set("Сетка не построена — смотрите журнал")
                            messagebox.showerror("Не удалось построить сетку", "Подробности находятся в журнале окна.")
                    elif task == "texture":
                        self.texture_output = None
                        if value == 0 and texture_output is not None and texture_output.is_file():
                            self.status.set("Текстурированная модель GLB готова")
                            self.append_log(f"Текстурированная модель сохранена: {texture_output}\n")
                            messagebox.showinfo("Модель готова", f"Файл можно импортировать в 3D-программу:\n\n{texture_output}")
                            self.open_path_location(texture_output.parent)
                        elif value == 130 or self.cancel_requested:
                            self.status.set("Текстурирование остановлено; исходная сетка не изменена")
                        else:
                            self.status.set("GLB не создан — смотрите журнал")
                            messagebox.showerror("Не удалось создать GLB", "Подробности находятся в журнале окна.")
                    elif task == "a2" and value == 0:
                        self.status.set("Съёмка импортирована; точность и привязка требуют проверки")
                        self.open_a2_report()
                    elif value == 0:
                        self.status.set("Готово")
                        messagebox.showinfo("Готово", f"Результат сохранён в:\n{self.result_dir}")
                    elif value == 130:
                        self.status.set("Обработка остановлена")
                    else:
                        self.status.set("Ошибка — смотрите журнал")
                        messagebox.showerror("Обработка не завершена", "Подробности находятся в журнале окна.")
                    if self.close_after_cancel:
                        self.close_viewers()
                        self.root.destroy()
                        return
        except queue.Empty:
            pass
        self.root.after(100, self.read_events)

    def open_result(self) -> None:
        if not self.result_dir:
            return
        try:
            if os.name == "nt":
                os.startfile(str(self.result_dir))  # type: ignore[attr-defined]
            else:
                subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", str(self.result_dir)])
        except OSError as exc:
            messagebox.showerror("Не удалось открыть папку", str(exc))

    @staticmethod
    def open_path_location(path: Path) -> None:
        try:
            if os.name == "nt":
                os.startfile(str(path))  # type: ignore[attr-defined]
            else:
                subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", str(path)])
        except OSError:
            pass

    def open_preview(self) -> None:
        if self.result_dir and (self.result_dir / "capture/manifest.json").is_file():
            self.open_capture(self.result_dir)
            return
        cloud = self.result_cloud()
        if cloud is not None:
            self.open_cloud(cloud)
        else:
            messagebox.showerror("Облако не найдено", "Файл PLY в папке результата не найден.")

    def result_cloud(self) -> Path | None:
        if self.result_dir is None:
            return None
        for name in ("dense.ply", "sparse.ply", "cloud.ply"):
            path = self.result_dir / name
            if path.is_file():
                return path
        return None

    def choose_cloud(self) -> None:
        path = filedialog.askopenfilename(title="Открыть облако точек",
                                          filetypes=[("Облако точек PLY", "*.ply"), ("Все файлы", "*")])
        if path:
            self.open_cloud(Path(path))

    def open_cloud(self, cloud: Path, capture: dict | None = None) -> None:
        from viewer_host import ViewerHost, open_window
        host = None
        try:
            host = ViewerHost(cloud, capture=capture)
            if not open_window(host):
                raise OSError("Не найден браузер, способный открыть окно просмотра.")
            self.viewer_hosts.append(host)
        except (OSError, ValueError) as exc:
            if host is not None:
                host.close()
            messagebox.showerror("Не удалось открыть облако", str(exc))

    def close_viewers(self) -> None:
        for host in self.viewer_hosts:
            host.close()
        self.viewer_hosts.clear()

    def cancel(self) -> None:
        if self.process and self.cancel_file is not None:
            try:
                self.cancel_requested = True
                if self.current_task == "mesh":
                    # PyCOLMAP's Poisson routine has no cancellation token. It
                    # runs in a child process and writes to a temporary path,
                    # so termination cannot replace the input or final output.
                    self.process.terminate()
                else:
                    # Reconstruction and texturing workers poll this file and
                    # terminate their own external processes before returning.
                    self.cancel_file.write_text("cancel\n", encoding="ascii")
                self.status.set("Останавливаем обработку…")
                self.cancel_button.configure(state="disabled")
            except OSError as exc:
                messagebox.showerror("Не удалось остановить", str(exc))

    def close(self) -> None:
        if self.opening_capture:
            messagebox.showinfo("Проверка съёмки", "Дождитесь завершения проверки файлов.")
            return
        if self.installing:
            messagebox.showinfo("Установка", "Дождитесь завершения установки движка.")
            return
        if self.process:
            if messagebox.askyesno("Идёт обработка", "Остановить обработку и закрыть окно?"):
                self.close_after_cancel = True
                self.cancel()
            return
        self.close_viewers()
        self.root.destroy()


def main() -> int:
    if sys.argv[1:2] == ["--model-worker"]:
        for stream in (sys.stdout, sys.stderr):
            if stream is not None and hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
        from model_worker import main as model_main
        return model_main(sys.argv[2:])
    if sys.argv[1:2] == ["--a2-import-worker"]:
        for stream in (sys.stdout, sys.stderr):
            if stream is not None and hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace")
        from a2_import import main as a2_main
        return a2_main(sys.argv[2:])
    if sys.argv[1:2] == ["--hardware-opencl-probe"]:
        from hardware_profile import hardware_opencl_probe_main
        return hardware_opencl_probe_main(verify="--inventory-only" not in sys.argv[2:])
    if sys.argv[1:2] == ["--mesh-worker"]:
        # A frozen Windows executable starts a separate process for Poisson;
        # keeping the native mesher out of the GUI process keeps cancellation
        # and crashes isolated from the project window.
        for stream in (sys.stdout, sys.stderr):
            if stream is not None and hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace")
        from mesh_worker import main as mesh_main
        sys.argv = [sys.argv[0], *sys.argv[2:]]
        return mesh_main()
    if sys.argv[1:2] == ["--texture-worker"]:
        # The worker owns the OpenMVS child process so cancellation does not
        # leave TextureMesh running after the main window closes.
        for stream in (sys.stdout, sys.stderr):
            if stream is not None and hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace")
        from texture_worker import main as texture_main
        sys.argv = [sys.argv[0], *sys.argv[2:]]
        return texture_main()
    if sys.argv[1:2] == ["--worker"]:
        # A frozen Windows executable does not necessarily honor PYTHONUTF8.
        # The GUI reads this subprocess through a UTF-8 pipe.
        for stream in (sys.stdout, sys.stderr):
            if stream is not None and hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace")
        import reconstruct
        sys.argv = [sys.argv[0], *sys.argv[2:]]
        return reconstruct.main()
    if sys.argv[1:2] == ["--self-test"]:
        import pycolmap
        import PIL
        import preview
        import openmvs_install
        import colmap_cl_install
        import pyopencl
        import rawpy
        import exifread
        import piexif
        import mesh_worker
        import texture_worker
        import viewer_host
        import a2_import
        import laspy
        if not callable(getattr(pycolmap, "poisson_meshing", None)) or not hasattr(pycolmap, "PoissonMeshingOptions"):
            raise RuntimeError("В комплекте PyCOLMAP нет Poisson meshing, необходимого для построения 3D-сетки.")
        print(f"Spatial Photo Cloud: pycolmap {pycolmap.__version__}; Pillow {PIL.__version__}")
        return 0
    if sys.argv[1:2] == ["--viewer-self-test"]:
        from http.client import HTTPConnection
        from urllib.parse import urlsplit
        from viewer_host import ViewerHost
        with tempfile.TemporaryDirectory(prefix="spatial-viewer-test-") as directory:
            path = Path(directory) / "test.ply"
            path.write_bytes(b"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"
                             b"property float y\nproperty float z\nend_header\n0 0 0\n")
            host = ViewerHost(path)
            try:
                parsed = urlsplit(host.url)
                connection = HTTPConnection(parsed.hostname, parsed.port, timeout=5)
                try:
                    connection.request("GET", parsed.path)
                    response = connection.getresponse()
                    assert response.status == 200 and b"Spatial Photo Cloud" in response.read()
                    connection.request("GET", parsed.path + "cloud")
                    response = connection.getresponse()
                    assert response.status == 200 and response.read() == path.read_bytes()
                finally:
                    connection.close()
            finally:
                host.close()
        print("Spatial Photo Cloud: viewer host OK")
        return 0
    if sys.argv[1:2] == ["--raw-conversion-test"] and len(sys.argv) == 4:
        from raw_import import convert_cr2
        print(convert_cr2(Path(sys.argv[2]), Path(sys.argv[3])), flush=True)
        return 0
    if sys.argv[1:2] == ["--ui-smoke-test"]:
        window = tk.Tk()
        window.withdraw()
        App(window, auto_configure_on_start=False)
        window.update()
        window.destroy()
        print("Spatial Photo Cloud: GUI initialized")
        return 0
    window = tk.Tk()
    App(window)
    window.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
