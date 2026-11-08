from http.client import HTTPConnection
import json
import hashlib
from pathlib import Path
import shutil
import subprocess
import unittest
from urllib.parse import urlsplit

import test_a2_import
from capture_session import open_capture
from viewer_host import ViewerHost


class CaptureSessionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_a2_import.CaptureImportTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.csv('Name,timestamp,x,y,z\nPT1,1735725777.5,2170499.478,312016.336,240.424\n')
        self.fixture.run_import()
        self.root = self.fixture.output

    def test_reopen_with_exact_timestamps_and_controls(self):
        cloud, data = open_capture(self.root)
        self.assertEqual(cloud, self.root / 'cloud.ply')
        self.assertEqual(data['route'][0]['timestamp_ns'], '1735725777104061985')
        self.assertEqual(data['controls'][0]['name'], 'PT1')
        self.assertTrue(data['read_only'])

    def test_portable_directory(self):
        target = self.root.parent / 'moved'
        self.root.rename(target)
        self.assertEqual(open_capture(target / 'capture')[1]['pose_count'], 2)

    def test_modified_cloud_rejected(self):
        with (self.root / 'cloud.ply').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'PLY изменён'):
            open_capture(self.root)

    def test_modified_trajectory_rejected(self):
        (self.root / 'capture/trajectory.jsonl').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'Контрольная сумма'):
            open_capture(self.root)

    def test_old_bundle_requires_reimport(self):
        path = self.root / 'capture/manifest.json'
        manifest = json.loads(path.read_text())
        del manifest['working_cloud']['sha256_at_import']
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'старой съёмке'):
            open_capture(self.root)

    def test_missing_manifest_rejected(self):
        (self.root / 'capture/manifest.json').unlink()
        with self.assertRaises(ValueError):
            open_capture(self.root)

    def test_session_endpoint_and_write_protection(self):
        cloud, capture = open_capture(self.root)
        host = ViewerHost(cloud, capture=capture)
        self.addCleanup(host.close)
        url = urlsplit(host.url)
        connection = HTTPConnection(url.hostname, url.port)
        self.addCleanup(connection.close)
        connection.request('GET', url.path+'capture.json')
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(json.loads(response.read())['session_id'], capture['session_id'])
        connection.request('POST', url.path+'save', body=b'invalid')
        response = connection.getresponse()
        self.assertEqual(response.status, 403)
        response.read()
        connection.request('GET', url.path+'../capture/sources/cloud.las')
        response = connection.getresponse()
        self.assertEqual(response.status, 404)
        response.read()
        with cloud.open('ab') as stream:
            stream.write(b'changed')
        connection.request('GET', url.path+'cloud')
        response = connection.getresponse()
        self.assertEqual(response.status, 409)
        response.read()

    @unittest.skipUnless(shutil.which('node'), 'Node.js is not installed')
    def test_joint_viewer_scripts(self):
        cloud, capture = open_capture(self.root)
        host = ViewerHost(cloud, capture=capture)
        self.addCleanup(host.close)
        subprocess.run([shutil.which('node'), str(Path(__file__).with_name('test_full_viewer_app.cjs')),
                        host.url, str(cloud), 'capture'], check=True, timeout=30)

    def test_sampling_keeps_end_of_long_route_and_exact_time(self):
        path = self.root / 'capture/trajectory.jsonl'
        pose = json.loads(path.read_text().splitlines()[0])
        start = pose['timestamp_ns']
        with path.open('w') as stream:
            for index in range(5001):
                pose['timestamp_ns'] = start + index
                stream.write(json.dumps(pose) + '\n')
        manifest_path = self.root / 'capture/manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['trajectory']['pose_count'] = 5001
        for artifact in manifest['artifacts']:
            if artifact['path'] == 'trajectory.jsonl':
                artifact['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        data = open_capture(self.root)[1]
        self.assertLessEqual(len(data['route']), 2000)
        self.assertEqual(data['route'][-1]['index'], 5000)
        self.assertEqual(data['route'][-1]['timestamp_ns'], str(start + 5000))


if __name__ == '__main__':
    unittest.main()
