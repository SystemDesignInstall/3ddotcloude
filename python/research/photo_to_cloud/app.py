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


def worker_command(images: str, output: Path, mode: str, cancel_file: Path) -> list[str]:
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--worker"]
    else:
        command = [sys.executable, "-u", str(Path(__file__).with_name("reconstruct.py"))]
    return command + ["--images", images, "--output", str(output), "--mode", mode,
                      "--cancel-file", str(cancel_file)]


class App:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("Spatial Photo Cloud — эксперимент")
        root.geometry("820x760")
        root.minsize(700, 650)
        self.events: queue.Queue = queue.Queue()
        self.process: subprocess.Popen | None = None
        self.cancel_dir: tempfile.TemporaryDirectory | None = None
        self.cancel_file: Path | None = None
        self.installing = False
        self.close_after_cancel = False
        self.result_dir: Path | None = None
        self.viewer_hosts = []
        self.images = tk.StringVar()
        self.destination = tk.StringVar()
        self.openmvs = tk.StringVar()
        self.colmap = tk.StringVar()
        self.mode = tk.StringVar(value=next(iter(MODES)))
        self.status = tk.StringVar(value="Выберите фотографии и папку результата")

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
        ttk.Label(frame, text="Фотографии → облако точек", font=(title_family, 17, "bold")).pack(anchor="w")
        ttk.Label(frame, text="Локальный эксперимент: файлы не загружаются в интернет.").pack(anchor="w", pady=(4, 12))
        self.path_row(frame, "Папка с фотографиями (CR2, JPG, PNG, TIFF)", self.images, self.choose_images)
        self.path_row(frame, "Где сохранять результат", self.destination, self.choose_destination)
        ttk.Label(frame, text="Режим").pack(anchor="w", pady=(8, 2))
        ttk.Combobox(frame, state="readonly", values=list(MODES), textvariable=self.mode).pack(fill="x")
        self.path_row(frame, "Папка OpenMVS (для плотного облака на CPU)", self.openmvs, self.choose_openmvs)
        install_row = ttk.Frame(frame)
        install_row.pack(fill="x", pady=(3, 0))
        self.install_button = ttk.Button(install_row, text="Установить OpenMVS (22 МБ)", command=self.install_openmvs)
        self.install_button.pack(side="left")
        if os.name != "nt":
            self.install_button.configure(state="disabled")
        ttk.Label(install_row, text="OpenMVS: AGPL-3.0; загружается отдельно").pack(side="left", padx=8)
        self.path_row(frame, "COLMAP-CL для AMD OpenCL (или COLMAP с HIP)", self.colmap, self.choose_colmap)
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
        self.start_button = ttk.Button(actions, text="Построить облако", command=self.start)
        self.start_button.pack(side="left")
        self.open_button = ttk.Button(actions, text="Открыть папку результата", command=self.open_result, state="disabled")
        self.open_button.pack(side="left", padx=8)
        self.cancel_button = ttk.Button(actions, text="Остановить", command=self.cancel, state="disabled")
        self.cancel_button.pack(side="left", padx=8)
        viewer_actions = ttk.Frame(frame)
        viewer_actions.pack(fill="x", pady=(0, 8))
        self.preview_button = ttk.Button(viewer_actions, text="Открыть построенное облако", command=self.open_preview, state="disabled")
        self.preview_button.pack(side="left")
        ttk.Button(viewer_actions, text="Открыть готовый PLY…", command=self.choose_cloud).pack(side="left", padx=8)
        ttk.Label(frame, textvariable=self.status).pack(anchor="w", pady=(0, 4))
        self.progress = ttk.Progressbar(frame, mode="indeterminate")
        self.progress.pack(fill="x", pady=(0, 8))
        self.log = scrolledtext.ScrolledText(frame, height=11, state="disabled", wrap="word")
        self.log.pack(fill="both", expand=True)
        root.protocol("WM_DELETE_WINDOW", self.close)
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

    def choose_destination(self) -> None:
        value = filedialog.askdirectory(title="Папка для результатов")
        if value:
            self.destination.set(value)

    def choose_openmvs(self) -> None:
        value = filedialog.askdirectory(title="Папка с InterfaceCOLMAP и DensifyPointCloud")
        if value:
            self.openmvs.set(value)

    def choose_colmap(self) -> None:
        value = filedialog.askopenfilename(title="Исполняемый файл colmap.exe", filetypes=[("Программа", "*.exe"), ("Все файлы", "*")])
        if value:
            self.colmap.set(value)

    def install_openmvs(self) -> None:
        if self.installing or self.process:
            return
        self.installing = True
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
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
        if self.installing or self.process:
            return
        self.installing = True
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
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

    def append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self) -> None:
        if self.process or self.installing:
            return
        if not self.images.get().strip() or not self.destination.get().strip():
            messagebox.showerror("Не выбраны папки", "Выберите папку с фотографиями и папку результата.")
            return
        mode = MODES[self.mode.get()]
        if mode == "dense-cpu" and not self.openmvs.get().strip():
            messagebox.showerror("Нужен OpenMVS", "Выберите папку с программами OpenMVS.")
            return
        if mode in {"dense-amd", "dense-opencl"} and not self.colmap.get().strip():
            messagebox.showerror("Нужен движок COLMAP", "Установите или выберите подходящий colmap.exe.")
            return
        label = datetime.datetime.now().strftime("photo-cloud-%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8]
        self.result_dir = Path(self.destination.get()).expanduser().resolve() / label
        self.cancel_dir = tempfile.TemporaryDirectory(prefix="spatial-cancel-")
        self.cancel_file = Path(self.cancel_dir.name) / "cancel"
        self.close_after_cancel = False
        command = worker_command(self.images.get(), self.result_dir, mode, self.cancel_file)
        if mode == "dense-cpu":
            command += ["--openmvs-dir", self.openmvs.get()]
        elif mode in {"dense-amd", "dense-opencl"}:
            command += ["--colmap", self.colmap.get()]
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
            messagebox.showerror("Не удалось запустить", str(exc))
            return
        self.append_log(f"\nПапка результата: {self.result_dir}\n")
        self.start_button.configure(state="disabled")
        self.open_button.configure(state="disabled")
        self.preview_button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self.install_button.configure(state="disabled")
        self.install_cl_button.configure(state="disabled")
        self.status.set("Обработка…")
        self.progress.start(15)
        threading.Thread(target=self.consume, args=(self.process,), daemon=True).start()

    def consume(self, process: subprocess.Popen) -> None:
        assert process.stdout is not None
        for line in process.stdout:
            self.events.put(("line", line))
        self.events.put(("done", process.wait()))

    def read_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "line":
                    self.append_log(value)
                elif kind in {"install_done", "install_error", "install_cl_done", "install_cl_error"}:
                    self.installing = False
                    self.progress.stop()
                    self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
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
                else:
                    self.progress.stop()
                    self.process = None
                    if self.cancel_dir is not None:
                        self.cancel_dir.cleanup()
                    self.cancel_dir = None
                    self.cancel_file = None
                    self.start_button.configure(state="normal")
                    self.cancel_button.configure(state="disabled")
                    self.install_button.configure(state="normal" if os.name == "nt" else "disabled")
                    self.install_cl_button.configure(state="normal" if os.name == "nt" else "disabled")
                    if self.result_dir and self.result_dir.exists():
                        self.open_button.configure(state="normal")
                        if self.result_cloud() is not None and value == 0:
                            self.preview_button.configure(state="normal")
                    if value == 0:
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

    def open_preview(self) -> None:
        cloud = self.result_cloud()
        if cloud is not None:
            self.open_cloud(cloud)
        else:
            messagebox.showerror("Облако не найдено", "Файл PLY в папке результата не найден.")

    def result_cloud(self) -> Path | None:
        if self.result_dir is None:
            return None
        for name in ("dense.ply", "sparse.ply"):
            path = self.result_dir / name
            if path.is_file():
                return path
        return None

    def choose_cloud(self) -> None:
        path = filedialog.askopenfilename(title="Открыть облако точек",
                                          filetypes=[("Облако точек PLY", "*.ply"), ("Все файлы", "*")])
        if path:
            self.open_cloud(Path(path))

    def open_cloud(self, cloud: Path) -> None:
        from viewer_host import ViewerHost, open_window
        host = None
        try:
            host = ViewerHost(cloud)
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
                self.cancel_file.write_text("cancel\n", encoding="ascii")
                self.status.set("Останавливаем обработку…")
                self.cancel_button.configure(state="disabled")
            except OSError as exc:
                messagebox.showerror("Не удалось остановить", str(exc))

    def close(self) -> None:
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
        import viewer_host
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
        App(window)
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
