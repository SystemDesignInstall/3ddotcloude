#!/usr/bin/env python3
"""Small desktop launcher for the experimental local reconstruction workflow."""

from __future__ import annotations

import datetime
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import uuid

import tkinter as tk
from tkinter import filedialog, font, messagebox, scrolledtext, ttk


MODES = {
    "Плотное облако — CPU (OpenMVS)": "dense-cpu",
    "Разреженное облако — CPU": "sparse-cpu",
    "Плотное облако — AMD HIP (эксперимент)": "dense-amd",
}


class App:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("Spatial Photo Cloud — эксперимент")
        root.geometry("760x630")
        root.minsize(650, 510)
        self.events: queue.Queue = queue.Queue()
        self.process: subprocess.Popen | None = None
        self.result_dir: Path | None = None
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
        self.path_row(frame, "Папка с фотографиями", self.images, self.choose_images)
        self.path_row(frame, "Где сохранять результат", self.destination, self.choose_destination)
        ttk.Label(frame, text="Режим").pack(anchor="w", pady=(8, 2))
        ttk.Combobox(frame, state="readonly", values=list(MODES), textvariable=self.mode).pack(fill="x")
        self.path_row(frame, "Папка OpenMVS (для плотного облака на CPU)", self.openmvs, self.choose_openmvs)
        self.path_row(frame, "COLMAP с HIP/ROCm (только для режима AMD)", self.colmap, self.choose_colmap)
        ttk.Label(frame, text="Масштаб результата по одним фотографиям неизвестен.").pack(anchor="w", pady=(10, 4))

        actions = ttk.Frame(frame)
        actions.pack(fill="x", pady=(0, 8))
        self.start_button = ttk.Button(actions, text="Построить облако", command=self.start)
        self.start_button.pack(side="left")
        self.open_button = ttk.Button(actions, text="Открыть папку результата", command=self.open_result, state="disabled")
        self.open_button.pack(side="left", padx=8)
        ttk.Label(actions, textvariable=self.status).pack(side="left", padx=8)
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
        value = filedialog.askopenfilename(title="Исполняемый файл COLMAP с HIP/ROCm")
        if value:
            self.colmap.set(value)

    def append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self) -> None:
        if self.process:
            return
        if not self.images.get().strip() or not self.destination.get().strip():
            messagebox.showerror("Не выбраны папки", "Выберите папку с фотографиями и папку результата.")
            return
        mode = MODES[self.mode.get()]
        if mode == "dense-cpu" and not self.openmvs.get().strip():
            messagebox.showerror("Нужен OpenMVS", "Выберите папку с программами OpenMVS.")
            return
        if mode == "dense-amd" and not self.colmap.get().strip():
            messagebox.showerror("Нужен COLMAP с HIP", "Выберите исполняемый файл COLMAP с HIP/ROCm.")
            return
        label = datetime.datetime.now().strftime("photo-cloud-%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8]
        self.result_dir = Path(self.destination.get()).expanduser().resolve() / label
        command = [sys.executable, "-u", str(Path(__file__).with_name("reconstruct.py")),
                   "--images", self.images.get(), "--output", str(self.result_dir), "--mode", mode]
        if mode == "dense-cpu":
            command += ["--openmvs-dir", self.openmvs.get()]
        elif mode == "dense-amd":
            command += ["--colmap", self.colmap.get()]
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, encoding="utf-8", errors="replace", env=environment,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            messagebox.showerror("Не удалось запустить", str(exc))
            return
        self.append_log(f"\nПапка результата: {self.result_dir}\n")
        self.start_button.configure(state="disabled")
        self.open_button.configure(state="disabled")
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
                else:
                    self.progress.stop()
                    self.process = None
                    self.start_button.configure(state="normal")
                    if self.result_dir and self.result_dir.exists():
                        self.open_button.configure(state="normal")
                    if value == 0:
                        self.status.set("Готово")
                        messagebox.showinfo("Готово", f"Результат сохранён в:\n{self.result_dir}")
                    else:
                        self.status.set("Ошибка — смотрите журнал")
                        messagebox.showerror("Обработка не завершена", "Подробности находятся в журнале окна.")
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

    def close(self) -> None:
        if self.process:
            messagebox.showinfo("Идёт обработка", "Дождитесь завершения обработки, затем закройте окно.")
            return
        self.root.destroy()


if __name__ == "__main__":
    window = tk.Tk()
    App(window)
    window.mainloop()
