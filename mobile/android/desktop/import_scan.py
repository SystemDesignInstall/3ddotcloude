#!/usr/bin/env python3
"""Validate and unpack Spatial Capture v1; recover cloud from committed observations.
No network, no third-party packages, no writes to Spatial Platform Core/CAS.
"""
from __future__ import annotations
import argparse, gzip, hashlib, json, math, os, shutil, struct, tempfile, zipfile
from pathlib import Path, PurePosixPath

MAX_BYTES = 2 * 1024**3
MAX_POINTS = 300000
PAYLOADS = ('depth.u16le', 'confidence.u8', 'color.nv21.gz', 'color.jpg')

def read_json(path):
    if path.stat().st_size > 1024*1024: raise ValueError('Metadata exceeds 1 MiB')
    return json.loads(path.read_text(encoding='utf-8'))

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(65536), b''): h.update(chunk)
    return h.hexdigest()

def unpack(archive, target):
    with zipfile.ZipFile(archive) as z:
        if len(z.infolist()) > 12000: raise ValueError('Too many archive entries')
        total = 0; seen = set()
        for info in z.infolist():
            p = PurePosixPath(info.filename)
            if p.is_absolute() or '..' in p.parts or '\\' in info.filename or ':' in info.filename or not p.parts:
                raise ValueError('Unsafe archive path')
            if (info.external_attr >> 16) & 0o170000 == 0o120000: raise ValueError('Symlinks are forbidden')
            key = str(p).casefold()
            if key in seen: raise ValueError('Duplicate archive entry')
            seen.add(key); total += info.file_size
            if total > MAX_BYTES or info.file_size > 128*1024**2: raise ValueError('Archive exceeds size limit')
            dest = target.joinpath(*p.parts)
            if info.is_dir(): dest.mkdir(parents=True, exist_ok=True); continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, dest.open('xb') as out:
                remaining = info.file_size
                while remaining:
                    chunk = src.read(min(65536, remaining))
                    if not chunk: raise ValueError('Truncated ZIP entry')
                    out.write(chunk); remaining -= len(chunk)
                if src.read(1): raise ValueError('ZIP size mismatch')

def observations(root):
    previous = -1
    entries = sorted((root / 'frames').glob('*/frame.json'))
    if len(entries) > 901: raise ValueError('Too many frames')
    for expected, path in enumerate(entries):
        m = read_json(path); folder = path.parent
        if m['index'] != expected or folder.name != f'{expected:06d}': raise ValueError('Non-contiguous frames')
        if m['tracking_state'] != 'TRACKING': raise ValueError('Untracked observation')
        ct, dt = int(m['camera_timestamp_ns']), int(m['depth_timestamp_ns'])
        if ct <= previous or dt < 0: raise ValueError('Invalid timestamps')
        previous = ct
        w, h, cw, ch = (int(m[k]) for k in ('depth_width','depth_height','color_width','color_height'))
        if not all(0 < n <= 4096 for n in (w,h,cw,ch)) or cw%2 or ch%2: raise ValueError('Invalid image dimensions')
        for name in PAYLOADS:
            f = folder / name
            if not f.is_file() or digest(f) != m['sha256'][name]: raise ValueError(f'Checksum failed: {folder.name}/{name}')
        if (folder/'depth.u16le').stat().st_size != w*h*2 or (folder/'confidence.u8').stat().st_size != w*h:
            raise ValueError('Depth size mismatch')
        pose = m['world_from_camera_cv']; k = m['depth_intrinsics_fx_fy_cx_cy']; corners=m['texture_to_image_corners']
        if len(pose)!=16 or len(k)!=4 or len(corners)!=8 or not all(math.isfinite(v) for v in pose+k+corners) or min(k[:2])<=0:
            raise ValueError('Invalid calibration or transform')
        if any(abs(pose[i]-v)>1e-5 for i,v in ((3,0),(7,0),(11,0),(15,1))): raise ValueError('Non-affine pose')
        for a in range(3):
            for b in range(3):
                if abs(sum(pose[a*4+r]*pose[b*4+r] for r in range(3))-(a==b)) > .001: raise ValueError('Non-rigid pose')
        det=pose[0]*(pose[5]*pose[10]-pose[9]*pose[6])-pose[4]*(pose[1]*pose[10]-pose[9]*pose[2])+pose[8]*(pose[1]*pose[6]-pose[5]*pose[2])
        if abs(det-1)>.001: raise ValueError('Reflected pose')
        with gzip.open(folder/'color.nv21.gz','rb') as f:
            color=f.read(cw*ch*3//2+1)
        if len(color)!=cw*ch*3//2: raise ValueError('Invalid NV21 size')
        yield folder, m, color

def rgb(nv, w, h, x, y):
    x=max(0,min(w-1,x));y=max(0,min(h-1,y));a=298*max(0,nv[y*w+x]-16)
    off=w*h+(y//2)*w+(x//2)*2;v=nv[off]-128;u=nv[off+1]-128
    return tuple(max(0,min(255,n)) for n in ((a+409*v+128)>>8,(a-100*u-208*v+128)>>8,(a+516*u+128)>>8))

def rebuild(root, output):
    voxels={}
    for folder,m,nv in observations(root):
        d=(folder/'depth.u16le').read_bytes();q=(folder/'confidence.u8').read_bytes()
        w,h,cw,ch=(m[k] for k in ('depth_width','depth_height','color_width','color_height'))
        fx,fy,cx,cy=m['depth_intrinsics_fx_fy_cx_cy'];pose=m['world_from_camera_cv'];c=m['texture_to_image_corners']
        step=max(1,math.ceil(math.sqrt(w*h/12000)))
        for y in range(0,h,step):
            for x in range(0,w,step):
                i=y*w+x;mm=struct.unpack_from('<H',d,i*2)[0]
                if not 300<=mm<=5000 or q[i]<128: continue
                z=mm*.001;local=((x-cx)*z/fx,(y-cy)*z/fy,z)
                world=tuple(sum(pose[a*4+r]*local[a] for a in range(3))+pose[12+r] for r in range(3))
                key=tuple(math.floor(v/.025) for v in world)
                if key in voxels and voxels[key][-1]>=q[i]: continue
                if key not in voxels and len(voxels)>=MAX_POINTS: continue
                u,v=x/w,y/h
                ix,iy=(math.floor((1-u)*(1-v)*c[r]+u*(1-v)*c[2+r]+(1-u)*v*c[4+r]+u*v*c[6+r]+.5) for r in (0,1))
                if not (0<=ix<cw and 0<=iy<ch): continue
                voxels[key]=world+rgb(nv,cw,ch,ix,iy)+(q[i],)
    header=('ply\nformat binary_little_endian 1.0\ncomment units meters\ncomment frame arcore_session_world right_handed y_up\n'
            f'element vertex {len(voxels)}\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar confidence\nend_header\n')
    with output.open('wb') as f:
        f.write(header.encode('ascii'))
        for point in voxels.values(): f.write(struct.pack('<fffBBBB',*point))
    return len(voxels)

def import_scan(archive, output, recover=False):
    output=Path(output).resolve();archive=Path(archive)
    if output.exists(): raise ValueError('Output already exists; choose a new directory')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.spatial-import-',dir=output.parent) as temp:
        stage=Path(temp)/'result';raw=stage/'capture';raw.mkdir(parents=True);unpack(archive,raw)
        session=read_json(raw/'session.json')
        if session.get('format')!='spatial-capture' or session.get('format_version')!=1: raise ValueError('Unsupported capture format')
        if session.get('pose_direction')!='world_from_camera_cv' or session.get('matrix_layout')!='column-major' or session.get('distance_unit')!='meter': raise ValueError('Unsupported coordinate contract')
        images=stage/'images';images.mkdir();poses=[]
        for folder,m,_ in observations(raw):
            shutil.copyfile(folder/'color.jpg',images/f'{m["index"]:06d}.jpg')
            poses.append({k:m[k] for k in ('index','camera_timestamp_ns','depth_timestamp_ns','world_from_camera_cv','image_intrinsics_fx_fy_cx_cy','color_width','color_height')})
        manifest=read_json(raw/'manifest.json') if (raw/'manifest.json').exists() else None
        if manifest and (manifest.get('state')!='complete' or manifest.get('frame_count')!=len(poses)): raise ValueError('Manifest frame count/state mismatch')
        if manifest and not recover:
            if digest(raw/'cloud.ply')!=manifest['cloud_sha256']: raise ValueError('Cloud checksum failed')
            shutil.copyfile(raw/'cloud.ply',stage/'cloud.ply');points=manifest['point_count']
        else: points=rebuild(raw,stage/'cloud.ply')
        trajectory={'world_frame_id':session['world_frame_id'],'camera_frame_id':session['camera_frame_id'],'pose_direction':'world_from_camera_cv','matrix_layout':'column-major','time_domain':session['time_domain'],'metric_basis':session['metric_basis'],'poses':poses}
        (stage/'trajectory.json').write_text(json.dumps(trajectory,indent=2)+'\n',encoding='utf-8')
        report={'format':'spatial-capture-import-report','frames':len(poses),'points':points,'recovered':not manifest or recover,'source_sha256':digest(archive),'coordinate_frame':session['world_frame_id'],'core_imported':False,'note':'Portable staging output; no Scene/CAS registration or global trajectory correction performed.'}
        (stage/'import_report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        os.rename(stage,output)
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('output',type=Path);p.add_argument('--rebuild',action='store_true');a=p.parse_args()
    try: print(json.dumps(import_scan(a.archive,a.output,a.rebuild),indent=2))
    except (ValueError,OSError,KeyError,TypeError,zipfile.BadZipFile) as e:p.exit(1,f'Import failed: {e}\n')
if __name__=='__main__':main()
