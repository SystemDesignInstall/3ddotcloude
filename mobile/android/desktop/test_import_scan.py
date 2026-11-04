import gzip,hashlib,json,struct,tempfile,unittest,zipfile
from pathlib import Path
from import_scan import import_scan

class ImportTests(unittest.TestCase):
    def make(self,root,corrupt=False):
        session={'format':'spatial-capture','format_version':1,'pose_direction':'world_from_camera_cv','matrix_layout':'column-major','distance_unit':'meter','world_frame_id':'test/world','camera_frame_id':'test/camera','time_domain':'ARCore device monotonic','metric_basis':'estimated'}
        payload={'depth.u16le':struct.pack('<HHHH',1000,1000,0,1000),'confidence.u8':bytes([255,255,255,0]),'color.nv21.gz':gzip.compress(bytes([235]*4+[128]*2)),'color.jpg':b'jpeg-fixture'}
        meta={'index':0,'tracking_state':'TRACKING','camera_timestamp_ns':'1000000001','depth_timestamp_ns':'1000000001','depth_width':2,'depth_height':2,'color_width':2,'color_height':2,'depth_intrinsics_fx_fy_cx_cy':[2,2,0,0],'image_intrinsics_fx_fy_cx_cy':[2,2,0,0],'world_from_camera_cv':[1,0,0,0,0,-1,0,0,0,0,-1,0,10,2,3,1],'texture_to_image_corners':[0,0,2,0,0,2,2,2],'sha256':{k:hashlib.sha256(v).hexdigest() for k,v in payload.items()}}
        z=root/'scan.zip'
        with zipfile.ZipFile(z,'w') as f:
            f.writestr('session.json',json.dumps(session));f.writestr('frames/000000/frame.json',json.dumps(meta))
            for k,v in payload.items():f.writestr('frames/000000/'+k,v+b'x' if corrupt and k=='depth.u16le' else v)
        return z
    def test_recovery_geometry_color(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);z=self.make(p);out=p/'output';r=import_scan(z,out)
            self.assertEqual((r['frames'],r['points'],r['recovered']),(1,2,True))
            cloud=(out/'cloud.ply').read_bytes().split(b'end_header\n')[1]
            self.assertEqual(struct.unpack('<fffBBBB',cloud[:16]),(10,2,2,255,255,255,255))
            self.assertEqual(struct.unpack('<fffBBBB',cloud[16:]),(10.5,2,2,255,255,255,255))
            self.assertFalse(r['core_imported']);self.assertTrue((out/'capture/frames/000000/color.nv21.gz').exists())
    def test_corruption_atomic(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);z=self.make(p,True)
            with self.assertRaisesRegex(ValueError,'Checksum'):import_scan(z,p/'out')
            self.assertFalse((p/'out').exists())
    def test_zip_traversal(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);z=p/'bad.zip'
            with zipfile.ZipFile(z,'w') as f:f.writestr('../escape',b'bad')
            with self.assertRaisesRegex(ValueError,'Unsafe'):import_scan(z,p/'out')
            self.assertFalse((p/'escape').exists())
    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);out=p/'out';out.mkdir();(out/'keep').write_text('yes')
            with self.assertRaisesRegex(ValueError,'exists'):import_scan(self.make(p),out)
            self.assertEqual((out/'keep').read_text(),'yes')
if __name__=='__main__':unittest.main()
