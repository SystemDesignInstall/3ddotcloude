"""Create a small, self-contained offline preview of a PLY point cloud."""

from __future__ import annotations

import json
import math
from pathlib import Path
import shutil
import struct


_TYPES = {
    "char": ("b", 1), "int8": ("b", 1), "uchar": ("B", 1), "uint8": ("B", 1),
    "short": ("h", 2), "int16": ("h", 2), "ushort": ("H", 2), "uint16": ("H", 2),
    "int": ("i", 4), "int32": ("i", 4), "uint": ("I", 4), "uint32": ("I", 4),
    "float": ("f", 4), "float32": ("f", 4), "double": ("d", 8), "float64": ("d", 8),
}


def sample_ply(path: Path, limit: int = 30000) -> tuple[int, list[list[float]]]:
    """Read a bounded, evenly spaced sample without loading the whole cloud."""
    if limit < 1:
        raise ValueError("preview limit must be positive")
    with path.open("rb") as stream:
        if stream.readline().strip() != b"ply":
            raise ValueError("not a PLY file")
        fmt, count, element, properties = None, None, None, []
        for _ in range(10000):
            line = stream.readline(4096)
            if not line or len(line) == 4096:
                raise ValueError("incomplete PLY header")
            words = line.decode("ascii").split()
            if not words:
                continue
            if words[0] == "format":
                fmt = words[1]
            elif words[0] == "element":
                element = words[1]
                if element == "vertex":
                    count = int(words[2])
                elif count is None and int(words[2]) != 0:
                    raise ValueError("unsupported element before vertices")
            elif words[0] == "property" and element == "vertex":
                if words[1] == "list":
                    properties.append((words[4], words[3], words[2]))
                else:
                    properties.append((words[2], words[1], None))
            elif words[0] == "end_header":
                break
        else:
            raise ValueError("incomplete PLY header")
        if count is None or count < 1 or not {"x", "y", "z"}.issubset({p[0] for p in properties}):
            raise ValueError("no vertex coordinates")
        if fmt not in {"ascii", "binary_little_endian", "binary_big_endian"}:
            raise ValueError("unsupported PLY format")
        for _, value_type, count_type in properties:
            if value_type not in _TYPES or (count_type is not None and count_type not in _TYPES):
                raise ValueError("unsupported PLY property")
        endian = "<" if fmt == "binary_little_endian" else ">"
        stride = max(1, math.ceil(count / limit))
        samples: list[list[float]] = []
        end = path.stat().st_size
        for index in range(count):
            selected = index % stride == 0
            values = {}
            if fmt == "ascii":
                fields = stream.readline().split()
                position = 0
                for name, _, count_type in properties:
                    if count_type is None:
                        if selected:
                            values[name] = float(fields[position])
                        position += 1
                    else:
                        length = int(fields[position]); position += length + 1
                    if position > len(fields):
                        raise ValueError("truncated vertex")
            else:
                for name, value_type, count_type in properties:
                    if count_type is None:
                        code, size = _TYPES[value_type]
                        if selected:
                            data = stream.read(size)
                            if len(data) != size:
                                raise ValueError("truncated vertex")
                            values[name] = struct.unpack(endian + code, data)[0]
                        else:
                            stream.seek(size, 1)
                    else:
                        code, size = _TYPES[count_type]
                        data = stream.read(size)
                        if len(data) != size:
                            raise ValueError("truncated vertex list")
                        length = struct.unpack(endian + code, data)[0]
                        skip = length * _TYPES[value_type][1]
                        if length < 0 or stream.tell() + skip > end:
                            raise ValueError("truncated vertex list")
                        stream.seek(skip, 1)
                if stream.tell() > end:
                    raise ValueError("truncated vertex")
            if selected:
                xyz = [float(values[key]) for key in ("x", "y", "z")]
                if all(math.isfinite(value) for value in xyz):
                    rgb = [max(0, min(255, int(values.get(key, 180)))) for key in ("red", "green", "blue")]
                    samples.append(xyz + rgb)
    if not samples:
        raise ValueError("no finite preview points")
    return count, samples


def create_preview(ply: Path, output: Path, limit: int = 30000) -> int:
    total, samples = sample_ply(ply, limit)
    bounds = []
    for axis in range(3):
        values = sorted(point[axis] for point in samples)
        bounds.append((values[len(values) // 100], values[min(len(values) - 1, len(values) * 99 // 100)]))
    center = [(low + high) / 2 for low, high in bounds]
    scale = max(high - low for low, high in bounds) or 1.0
    data = [[round((p[i] - center[i]) / scale, 6) for i in range(3)] + p[3:] for p in samples]
    page = _HTML.replace("__POINTS__", json.dumps(data, separators=(",", ":")))
    page = page.replace("__COUNT__", f"{len(samples):,} из {total:,}".replace(",", " "))
    output.write_text(page, encoding="utf-8")
    # Browsers cannot read a sibling local PLY without a user-selected file.
    # Include a standalone viewer which asks the user to select it locally.
    shutil.copyfile(Path(__file__).with_name("full_viewer.html"), output.with_name("full_viewer.html"))
    return len(samples)


_HTML = """<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Spatial Photo Cloud — просмотр</title>
<style>*{box-sizing:border-box}body{margin:0;background:#101923;color:#e7eef5;font:15px system-ui,sans-serif}
header{padding:14px 20px;background:#192a38}h1{font-size:18px;margin:0 0 5px}p{margin:0;color:#b8c9d5}a{color:#82d9f2}
canvas{display:block;width:100vw;height:calc(100vh - 104px);touch-action:none;cursor:grab}canvas:active{cursor:grabbing}</style></head>
<body><header><h1>Облако точек — предварительный просмотр</h1>
<p>Показано __COUNT__ точек. Вращение: мышь или палец; масштаб: колесо.</p>
<p><a href="full_viewer.html">Открыть полный просмотр</a> и выбрать файл dense.ply.</p></header>
<canvas id="view"></canvas><script>
const points=__POINTS__, canvas=document.getElementById('view'), ctx=canvas.getContext('2d');
let yaw=.35,pitch=-.2,zoom=1,drag=null,scheduled=false;
function draw(){scheduled=false;const d=window.devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;
 canvas.width=Math.round(w*d);canvas.height=Math.round(h*d);ctx.setTransform(d,0,0,d,0,0);ctx.fillStyle='#101923';ctx.fillRect(0,0,w,h);
 const s=Math.min(w,h)*.82*zoom,cy=Math.cos(yaw),sy=Math.sin(yaw),cp=Math.cos(pitch),sp=Math.sin(pitch);
 for(const p of points){const x=cy*p[0]-sy*p[2],z=sy*p[0]+cy*p[2],y=cp*p[1]-sp*z;
  const px=w/2+x*s,py=h/2-y*s;if(px<0||py<0||px>=w||py>=h)continue;
  ctx.fillStyle=`rgb(${p[3]},${p[4]},${p[5]})`;ctx.fillRect(px,py,1.6,1.6);}}
function refresh(){if(!scheduled){scheduled=true;requestAnimationFrame(draw)}}
canvas.addEventListener('pointerdown',e=>{drag=[e.clientX,e.clientY];canvas.setPointerCapture(e.pointerId)});
canvas.addEventListener('pointermove',e=>{if(!drag)return;yaw+=(e.clientX-drag[0])*.008;pitch+=(e.clientY-drag[1])*.008;drag=[e.clientX,e.clientY];refresh()});
canvas.addEventListener('pointerup',()=>drag=null);canvas.addEventListener('pointercancel',()=>drag=null);
canvas.addEventListener('wheel',e=>{e.preventDefault();zoom=Math.max(.15,Math.min(12,zoom*Math.exp(-e.deltaY*.001)));refresh()},{passive:false});
window.addEventListener('resize',refresh);refresh();</script></body></html>"""
