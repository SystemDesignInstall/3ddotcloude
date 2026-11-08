"""Capture import options; no processing on the Tk event loop."""
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


def show_dialog(parent, destination: str, submit) -> None:
    window = tk.Toplevel(parent)
    window.title("Импорт съёмки A2 mapper")
    window.transient(parent)
    window.grab_set()
    window.resizable(False, False)
    frame = ttk.Frame(window, padding=18)
    frame.pack(fill="both", expand=True)
    values = {key: tk.StringVar(value=default) for key, default in (
        ("source", ""), ("destination", destination), ("controls", ""),
        ("coordinate_frame", "a2_export_local_unverified"), ("source_version", "unknown"),
        ("rgb_encoding", "uint16"), ("up_axis", "z"))}
    ttk.Label(frame, text="Импорт облака и траектории", font=("TkDefaultFont", 14, "bold")).pack(anchor="w")
    ttk.Label(frame, text="Нужны готовые cloud.las и trajectory.txt из A2 mapper.", wraplength=540).pack(anchor="w", pady=(6, 10))

    def path_row(label, key, file=False):
        ttk.Label(frame, text=label).pack(anchor="w", pady=(6, 2))
        row = ttk.Frame(frame)
        row.pack(fill="x")
        ttk.Entry(row, textvariable=values[key], width=62).pack(side="left", fill="x", expand=True)
        def choose():
            value = (filedialog.askopenfilename(parent=window, title=label, filetypes=[("CSV", "*.csv")])
                     if file else filedialog.askdirectory(parent=window, title=label))
            if value:
                values[key].set(value)
        ttk.Button(row, text="Выбрать…", command=choose).pack(side="left", padx=(8, 0))

    path_row("Папка результатов A2 mapper", "source")
    path_row("Куда сохранить отдельную копию съёмки", "destination")
    path_row("Опорные точки CSV (необязательно; поиск рядом с LAS)", "controls", True)
    for label, key in (("Имя системы координат (название не подтверждает привязку)", "coordinate_frame"),
                       ("Версия A2 mapper (unknown, если неизвестна)", "source_version")):
        ttk.Label(frame, text=label).pack(anchor="w", pady=(6, 2))
        ttk.Entry(frame, textvariable=values[key]).pack(fill="x")
    options = ttk.Frame(frame)
    options.pack(fill="x", pady=10)
    ttk.Label(options, text="RGB в LAS").pack(side="left")
    ttk.Combobox(options, textvariable=values["rgb_encoding"], values=("uint16", "uint8"), width=9, state="readonly").pack(side="left", padx=8)
    ttk.Label(options, text="Вертикаль для просмотра").pack(side="left")
    ttk.Combobox(options, textvariable=values["up_axis"], values=("z", "y"), width=4, state="readonly").pack(side="left", padx=8)
    ttk.Label(frame, text="Исходные координаты сохраняются. Импорт не исправляет траекторию и не подтверждает точность. RGB uint16 — стандартный диапазон LAS; uint8 выбирайте только для соответствующего экспорта.", wraplength=540).pack(anchor="w", pady=(0, 10))

    def start():
        data = {key: variable.get().strip() for key, variable in values.items()}
        if not data["source"] or not Path(data["source"]).is_dir() or not data["destination"] or not Path(data["destination"]).is_dir():
            messagebox.showerror("Не выбраны папки", "Выберите существующие папки источника и результата.", parent=window)
            return
        if not data["coordinate_frame"] or not data["source_version"]:
            messagebox.showerror("Не заполнены параметры", "Укажите систему координат и версию источника.", parent=window)
            return
        if submit(data):
            window.destroy()
    ttk.Button(frame, text="Импортировать съёмку", command=start).pack(anchor="e")
