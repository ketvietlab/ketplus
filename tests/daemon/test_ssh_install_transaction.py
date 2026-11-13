"""ADR 0123 filesystem/rollback tests. No SSH service, account or provider is used."""
import base64
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import signal
import time
import tarfile
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'modules/remote/src/ssh/install/transaction.py'
spec = importlib.util.spec_from_file_location('transaction', SOURCE)
transaction = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transaction)


class InstallerTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='team5-ssh-install-test-')
        self.home = Path(self.temp.name)
        self.env = dict(os.environ, HOME=str(self.home), XDG_DATA_HOME=str(self.home / 'data'),
                        XDG_CONFIG_HOME=str(self.home / 'config'),
                        XDG_RUNTIME_DIR=str(self.home / 'runtime'),
                        KETPLUS_DAEMON_DATA_DIR=str(self.home / 'data/Ketsuite/KetPlus'),
                        KETPLUS_DAEMON_STATE=str(self.home / 'data/Ketsuite/KetPlus/daemon/journal.sqlite3'),
                        KETPLUS_DAEMON_SOCKET=str(self.home / 'daemon.sock'),
                        KETPLUS_DAEMON_TOKEN_FILE=str(self.home / 'data/Ketsuite/KetPlus/daemon/local.token'))
        self.root = self.home / '.local/opt/ketplus'

    def tearDown(self):
        # Only the QA child whose pid this fixture created; never search for daemon names.
        pid_file = self.home / 'qa-child.pid'
        if pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.temp.cleanup()

    def call(self, description, input=b''):
        result = subprocess.run([sys.executable, str(SOURCE),
                    base64.b64encode(json.dumps(description).encode()).decode()],
                    env=self.env, input=input, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, timeout=15)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        return result, rows

    def preflight(self):
        result, rows = self.call({'action': 'preflight', 'socket': str(self.home / 'daemon.sock')})
        self.assertEqual(result.returncode, 0, rows)
        return rows[0]

    def archive(self, version='1.2.3', bad_runtime=False, members=None):
        info = {'product_version': version, 'build_revision': 'a' * 40,
                'control_url': 'http://127.0.0.1:9'}
        daemon = ("#!/usr/bin/env python3\nimport sys,time,os,socket,signal\nfrom pathlib import Path\n"
                  "if '--build-info' in sys.argv: print(" + repr(json.dumps(info)) + ");sys.exit()\n"
                  "if '--check-runtime' in sys.argv: sys.exit(" + str(int(bad_runtime)) + ")\n"
                  "p=Path(os.environ['HOME'])/'qa-child.pid';p.write_text(str(os.getpid()))\n"
                  "s=socket.socket(socket.AF_UNIX);path=os.environ['KETPLUS_DAEMON_SOCKET']\n"
                  "s.bind(path);s.listen(1);s.settimeout(.1)\n"
                  "def stop(*args): raise SystemExit()\n"
                  "signal.signal(signal.SIGTERM,stop)\n"
                  "try:\n"
                  " while True:\n"
                  "  try:\n"
                  "   c,_=s.accept();c.close();break\n"
                  "  except socket.timeout: pass\n"
                  "finally:\n s.close();Path(path).unlink(missing_ok=True);p.unlink(missing_ok=True)\n").encode()
        cli = ("#!/usr/bin/env python3\nimport os,sys,socket,json\nfrom pathlib import Path\n"
               "if (Path(os.environ['HOME'])/'busy').exists(): sys.exit(1)\n"
               "s=socket.socket(socket.AF_UNIX);s.connect(os.environ['KETPLUS_DAEMON_SOCKET']);s.close()\n"
               "print(json.dumps({'ok':True}))\n").encode()
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as archive:
            for name, data in members or [('bin/ketplus_daemon', daemon),
                                          ('bin/ketplusctl', cli)]:
                item = tarfile.TarInfo(name)
                item.size = len(data)
                item.mode = 0o755
                archive.addfile(item, io.BytesIO(data))
        return output.getvalue()

    def request(self, archive, action='install', version='1.2.3'):
        preflight = self.preflight()
        return {'action': action, 'socket': str(self.home / 'daemon.sock'),
                'profile': preflight['profile'], 'installation': preflight['installation'],
                'artifact': {'version': version, 'revision': 'a' * 40, 'architecture': 'x86_64',
                    'control_url': 'http://127.0.0.1:9', 'bytes': len(archive),
                    'sha256': hashlib.sha256(archive).hexdigest()}}

    def installed(self):
        archive = self.archive()
        result, rows = self.call(self.request(archive), archive + b'commit\n')
        self.assertEqual(result.returncode, 0, rows)
        for _ in range(100):
            if (self.home / 'daemon.sock').exists():
                break
            time.sleep(.02)
        self.assertTrue((self.home / 'daemon.sock').exists())

    def test_committed_install_and_idle_uninstall_retain_data(self):
        self.installed()
        marker = Path(self.env['KETPLUS_DAEMON_DATA_DIR']) / 'keep'
        marker.write_text('conversation data')
        request = self.request(b'', action='uninstall')
        result, rows = self.call(request, b'commit\n')
        self.assertEqual(result.returncode, 0, rows)
        self.assertFalse((self.root / 'current').exists())
        self.assertFalse((self.root / 'ssh-install.json').exists())
        self.assertEqual(marker.read_text(), 'conversation data')

    def test_busy_daemon_refuses_uninstall_without_changing_receipt(self):
        self.installed()
        before = (self.root / 'ssh-install.json').read_bytes()
        (self.home / 'busy').write_text('active turn')
        result, rows = self.call(self.request(b'', action='uninstall'), b'commit\n')
        self.assertNotEqual(result.returncode, 0, rows)
        self.assertEqual((self.root / 'ssh-install.json').read_bytes(), before)
        self.assertTrue((self.home / 'daemon.sock').exists())

    def test_upgrade_rollback_restores_receipt_and_user_data(self):
        self.installed()
        before = (self.root / 'ssh-install.json').read_bytes()
        data = Path(self.env['KETPLUS_DAEMON_DATA_DIR'])
        (data / 'keep').write_text('original')
        archive = self.archive(version='1.2.4')
        result, rows = self.call(self.request(archive, action='upgrade', version='1.2.4'), archive)
        self.assertNotEqual(result.returncode, 0, rows)
        self.assertIn({'phase': 'ready'}, rows)
        self.assertEqual((self.root / 'ssh-install.json').read_bytes(), before)
        self.assertEqual((data / 'keep').read_text(), 'original')
        self.assertEqual(len(list(self.root.glob('1.2.4-*'))), 0)
        for _ in range(100):
            if (self.home / 'daemon.sock').exists():
                break
            time.sleep(.02)
        self.assertTrue((self.home / 'daemon.sock').exists())

    def test_downgrade_refused_before_stopping_managed_daemon(self):
        self.installed()
        archive = self.archive(version='1.2.2')
        result, rows = self.call(self.request(archive, action='upgrade', version='1.2.2'), archive)
        self.assertNotEqual(result.returncode, 0, rows)
        self.assertTrue((self.home / 'daemon.sock').exists())

    def test_preflight_creates_nothing(self):
        self.assertFalse(self.preflight()['installed'])
        self.assertEqual(list(self.home.iterdir()), [])

    def test_unknown_installation_is_never_overwritten(self):
        self.root.mkdir(parents=True)
        marker = self.root / 'user-file'
        marker.write_text('keep')
        result, rows = self.call({'action': 'preflight', 'socket': str(self.home / 'daemon.sock')})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unmanaged', rows[0]['error'])
        self.assertEqual(marker.read_text(), 'keep')

    def test_bad_hash_and_partial_upload_leave_profile_unchanged(self):
        archive = self.archive()
        request = self.request(archive)
        for data in [archive[:-1], b'x' * len(archive)]:
            result, rows = self.call(request, data)
            self.assertNotEqual(result.returncode, 0, rows)
            self.assertFalse(self.root.exists(), rows)
            self.assertEqual(list(self.home.iterdir()), [])

    def test_failed_runtime_does_not_activate(self):
        archive = self.archive(bad_runtime=True)
        result, rows = self.call(self.request(archive), archive + b'commit\n')
        self.assertNotEqual(result.returncode, 0, rows)
        self.assertFalse(self.root.exists())

    def test_eof_after_ready_rolls_back(self):
        archive = self.archive()
        result, rows = self.call(self.request(archive), archive)
        self.assertNotEqual(result.returncode, 0, rows)
        self.assertIn({'phase': 'ready'}, rows)
        self.assertFalse(self.root.exists())

    def test_preexisting_version_directory_is_preserved_on_failure(self):
        archive = self.archive()
        request = self.request(archive)
        self.root.mkdir(parents=True)
        marker = self.root / ('1.2.3-' + 'a' * 40 + '-x86_64')
        marker.mkdir()
        (marker / 'keep').write_text('untouched')
        result, rows = self.call(request, archive)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((marker / 'keep').read_text(), 'untouched')

    def test_profile_change_invalidates_consent(self):
        archive = self.archive()
        request = self.request(archive)
        request['profile'] = 'wrong-profile'
        result, _ = self.call(request, archive)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.root.exists())

    def test_unsafe_archives_are_rejected_before_extracting(self):
        for name in ['../escape', '/absolute', 'bin/../../escape']:
            with self.subTest(name=name):
                archive = self.home / 'package.tar.gz'
                archive.write_bytes(self.archive(members=[(name, b'data')]))
                destination = self.home / 'unpacked'
                destination.mkdir(exist_ok=True)
                with self.assertRaises(transaction.Refused):
                    transaction.safe_extract(archive, destination)
                self.assertEqual(list(destination.iterdir()), [])

    def test_symlink_traversal_hardlinks_and_devices_are_rejected(self):
        for kind, target in [(tarfile.SYMTYPE, '../../escape'),
                             (tarfile.LNKTYPE, 'bin/ketplus_daemon'), (tarfile.FIFOTYPE, '')]:
            archive = self.home / 'package.tar.gz'
            with tarfile.open(archive, 'w:gz') as package:
                member = tarfile.TarInfo('lib/link')
                member.type = kind
                member.linkname = target
                package.addfile(member)
            destination = self.home / 'unpacked'
            destination.mkdir(exist_ok=True)
            with self.assertRaises(transaction.Refused):
                transaction.safe_extract(archive, destination)
            self.assertEqual(list(destination.iterdir()), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
