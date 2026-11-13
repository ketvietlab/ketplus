# MPL-2.0. ADR 0124. Sent verbatim to python3 over the host-key-verified SSH route.
# No shell interpolation, sudo, system service, provider login or relay enrolment.
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time


class Refused(Exception):
    pass


def require(ok, message):
    if not ok:
        raise Refused(message)


def owned(path, directory=False):
    info = path.lstat()
    require(info.st_uid == os.getuid() and not stat.S_ISLNK(info.st_mode),
            'An installation path is not owned by this account or is a symbolic link.')
    require(not info.st_mode & 0o022, 'An installation path is writable by another account.')
    require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
            'An installation path has an unexpected type.')


def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)


def run(args, env=None):
    # Error output may contain paths or account data. Return only a fixed error to Desktop.
    result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
    require(result.returncode == 0 and len(result.stdout) < 1024 * 1024,
            'The daemon runtime check or idle-only operation failed. No active turn was stopped.')
    return result.stdout


def version(value):
    parts = value.split('.')
    require(len(parts) == 3 and all(x.isdigit() for x in parts), 'Unsupported release version.')
    return tuple(int(x) for x in parts)


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(65536), b''):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        require(len(result) < 50000, 'The managed runtime has too many entries.')
        info = path.lstat()
        require(info.st_uid == os.getuid(), 'A managed runtime entry changed ownership.')
        name = str(path.relative_to(directory))
        if stat.S_ISLNK(info.st_mode):
            require(path.resolve().is_relative_to(directory.resolve()), 'A managed runtime link escapes.')
            result[name] = ['link', os.readlink(path)]
        elif stat.S_ISDIR(info.st_mode):
            result[name] = ['directory', stat.S_IMODE(info.st_mode)]
        else:
            require(stat.S_ISREG(info.st_mode), 'A managed runtime entry changed type.')
            result[name] = ['file', stat.S_IMODE(info.st_mode), file_hash(path)]
    return result


def peer_pid(path, timeout=5):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(timeout)
        peer.connect(str(path))
        pid, uid, _ = struct.unpack('3i', peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    require(uid == os.getuid(), 'The daemon socket belongs to another account.')
    return pid


def safe_extract(archive, destination):
    # Validate the complete table before creating any extracted entry. The publisher puts
    # bin/, lib/, plugins/, etc. directly at the archive root (no CPack wrapper directory).
    with tarfile.open(archive, 'r:gz') as package:
        entries = []
        names = set()
        total = 0
        links = set()
        for item in package:
            entries.append(item)
            require(len(entries) <= 50000, 'The runtime archive has too many entries.')
            name = PurePosixPath(item.name)
            require(not name.is_absolute() and '..' not in name.parts and
                    str(name) not in ('', '.') and str(name) not in names,
                    'Unsafe or duplicate archive path.')
            require(item.isdir() or item.isfile() or item.issym(), 'Unsupported archive entry.')
            require(not item.mode & 0o7000, 'Privileged archive permissions are forbidden.')
            require(all(str(PurePosixPath(*name.parts[:i])) not in links
                        for i in range(1, len(name.parts))), 'Archive entry traverses a link.')
            if item.issym():
                target = PurePosixPath(item.linkname)
                require(not target.is_absolute(), 'Absolute archive link.')
                resolved = list(name.parent.parts)
                for part in target.parts:
                    if part == '..':
                        require(bool(resolved), 'Archive link escapes the runtime.')
                        resolved.pop()
                    elif part != '.':
                        resolved.append(part)
                require(bool(resolved), 'Invalid archive link.')
                links.add(str(name))
            names.add(str(name))
            total += item.size
            require(total <= 2 * 1024 ** 3, 'The expanded runtime exceeds 2 GiB.')
        # Also reject a link introduced after its child in the archive table.
        require(all(not any(str(PurePosixPath(n).parent).startswith(link + '/') or
                            str(PurePosixPath(n).parent) == link for link in links)
                    for n in names), 'Archive entry traverses a link.')
        require('bin/ketplus_daemon' in names and 'bin/ketplusctl' in names,
                'The archive is not a complete daemon runtime.')
        for item in entries:
            target = destination / item.name
            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            if item.isdir():
                target.mkdir(exist_ok=True, mode=0o700)
            elif item.issym():
                target.symlink_to(item.linkname)
            else:
                with package.extractfile(item) as source, target.open('xb') as out:
                    shutil.copyfileobj(source, out, 65536)
                target.chmod(0o700 if item.mode & 0o111 else 0o600)
        for link in links:
            require((destination / link).resolve().is_relative_to(destination.resolve()),
                    'Archive link escapes the runtime.')


class Transaction:
    def __init__(self, description):
        self.description = description
        self.home = Path(os.environ['HOME'])
        owned(self.home, True)
        self.root = self.home / '.local/opt/ketplus'
        self.current = self.root / 'current'
        self.receipt = self.root / 'ssh-install.json'
        self.data = Path(os.environ.get('KETPLUS_DAEMON_DATA_DIR') or
                         str(Path(os.environ.get('XDG_DATA_HOME') or self.home / '.local/share') /
                             'Ketsuite/KetPlus'))
        # Custom daemon.env needs manual installation: do not reinterpret shell/environment
        # syntax, and do not silently replace an existing service's configuration.
        require(not (self.home / '.config/ketplus/daemon.env').exists(),
                'Custom daemon.env is present. Manage this installation manually.')
        for config in {self.home / '.config', Path(os.environ.get('XDG_CONFIG_HOME') or self.home / '.config')}:
            require(not (config / 'systemd/user/ketplus-daemon.service').exists(),
                    'An existing user service must be managed manually, not replaced by an SSH process.')
        self.socket = Path(description['socket'])
        self.token = Path(os.environ.get('KETPLUS_DAEMON_TOKEN_FILE') or
                          str(self.data / 'daemon/local.token'))
        self.state = Path(os.environ.get('KETPLUS_DAEMON_STATE') or
                          str(self.data / 'daemon/journal.sqlite3'))
        self.env = dict(os.environ, KETPLUS_DAEMON_DATA_DIR=str(self.data),
                        KETPLUS_DAEMON_SOCKET=str(self.socket),
                        KETPLUS_DAEMON_TOKEN_FILE=str(self.token),
                        KETPLUS_DAEMON_STATE=str(self.state),
                        KETPLUS_DISABLE_DAEMON_AUTOSTART='1')
        # A profile is identified during preflight and must not change after consent.
        self.profile = hashlib.sha256(json.dumps([str(self.home), str(self.root), str(self.data),
                         str(self.socket), str(self.token), str(self.state)]).encode()).hexdigest()
        self.created = []
        self.stage = None
        self.child = None
        self.previous = None
        self.previous_receipt = None
        self.snapshot = None
        self.committed = False
        self.activated = False
        self.stopped = False
        self.old = None
        self.old_pid = None
        self.new = None
        self.lock = None
        self.created_token = False
        require(not any('..' in path.parts for path in
                        (self.home, self.data, self.state, self.token, self.socket)),
                'Paths with parent traversal must be managed manually.')
        require(self.state.is_relative_to(self.data),
                'State files outside the daemon data directory must be managed manually.')

    def check_parents(self, path):
        require(path.is_absolute(), 'The account profile contains a relative path.')
        runtime = Path('/run/user') / str(os.getuid())
        if path == self.socket.parent and path.is_relative_to(runtime):
            for parent in [path, *path.parents]:
                owned(parent, True)
                if parent == runtime:
                    return
        for parent in [path, *path.parents]:
            if parent == self.home.parent:
                break
            if parent.exists() or parent.is_symlink():
                owned(parent, True)
        require(path.is_relative_to(self.home) or path == self.socket.parent,
                'The installer cannot modify a data path outside this home profile.')

    def preflight(self):
        self.old = None
        self.previous = None
        require(platform.system() == 'Linux', 'Only Linux hosts are supported.')
        arch = platform.machine()
        require(arch in ('x86_64', 'aarch64'), 'Unsupported Linux architecture.')
        for path in (self.root, self.data, self.state.parent, self.token.parent, self.socket.parent):
            self.check_parents(path)
        for path in (self.token, self.state):
            if path.exists() or path.is_symlink():
                owned(path)
        if self.root.exists():
            owned(self.root, True)
            if self.receipt.exists():
                owned(self.receipt)
                require(self.receipt.stat().st_size <= 16 * 1024 ** 2, 'The install receipt is oversized.')
                self.old = json.loads(self.receipt.read_text())
                require(self.old.get('schema') == 1 and self.old.get('profile') == self.profile,
                        'The existing installer receipt does not match this profile.')
                name = self.old['directory']
                require('/' not in name and name not in ('', '.', '..'), 'Invalid install receipt.')
                self.previous = self.root / name
                owned(self.previous, True)
                require(self.current.is_symlink() and os.readlink(self.current) == name,
                        'The active installation was changed outside this installer.')
            else:
                require(not any(self.root.iterdir()), 'An unmanaged installation already exists.')
        if self.socket.exists():
            require(self.old is not None, 'A daemon is already running outside this installer.')
            info = self.socket.lstat()
            require(stat.S_ISSOCK(info.st_mode) and info.st_uid == os.getuid(),
                    'The daemon socket is not owned by this account.')
            pid = peer_pid(self.socket)
            command = Path('/proc') / str(pid) / 'cmdline'
            require(str(self.previous / 'bin/ketplus_daemon').encode() in command.read_bytes().split(b'\0')[:2],
                    'The socket belongs to a different daemon. Nothing was changed.')
            self.old_pid = pid
        require(not self.socket.is_symlink(), 'The daemon socket is a symbolic link.')
        if self.old:
            require(inventory(self.previous) == self.old.get('inventory'),
                    'The managed runtime contains changed or unrecognised files. Resolve it manually.')
            # Detect an external change to either executable before calling or starting it.
            for binary, expected in self.old['binaries'].items():
                path = self.previous / 'bin' / binary
                owned(path)
                require(file_hash(path) == expected,
                        'The managed daemon runtime was modified. Resolve it manually.')
        return {'arch': arch, 'profile': self.profile, 'installed': self.old is not None,
                'version': self.old['version'] if self.old else '',
                'installation': hashlib.sha256(self.receipt.read_bytes() if self.old else b'').hexdigest(),
                'running': self.socket.exists()}

    def mkdir(self, path):
        if not path.exists():
            self.mkdir(path.parent)
            path.mkdir(mode=0o700)
            self.created.append(path)
        owned(path, True)

    def stop_old(self, replacement=None):
        if not self.socket.exists() and self.old_pid is None:
            return
        if self.socket.exists():
            cli = str(self.previous / 'bin/ketplusctl')
            operation = 'daemon.prepare_upgrade' if replacement else 'daemon.stop_if_idle'
            payload = json.dumps({'replacement_version': replacement}) if replacement else '{}'
            result = json.loads(run([cli, '--timeout', '20', 'call', operation, payload], self.env))
            require(result.get('ok') is True, 'The daemon refused to stop while busy.')
        self.stopped = True
        for _ in range(100):
            try:
                status = (Path('/proc') / str(self.old_pid) / 'stat').read_text()
                exited = status.rpartition(') ')[2].split()[0] == 'Z'
            except FileNotFoundError:
                exited = True
            if not self.socket.exists() and exited:
                return
            time.sleep(0.1)
        raise Refused('The idle daemon did not stop in time.')

    def start(self, directory):
        self.mkdir(self.data)
        self.mkdir(self.token.parent)
        self.mkdir(self.state.parent)
        self.mkdir(self.socket.parent)
        if not self.token.exists():
            with self.token.open('xb') as token:
                self.created_token = True
                value = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b'=')
                require(token.write(value) == len(value), 'Unable to initialise the private local credential.')
        # No service manager, shell, login, provider invocation or remote enrolment is involved.
        # Existing user data is kept in place; no credentials cross this SSH channel.
        child = subprocess.Popen([str(directory / 'bin/ketplus_daemon'), '--socket', str(self.socket),
                     '--state', str(self.state), '--token-file', str(self.token)],
                     env=self.env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
        return child

    def activate(self):
        initial = self.preflight()
        require(self.description['profile'] == self.profile, 'The account profile changed after consent.')
        require(self.description['installation'] == initial['installation'],
                'The managed installation changed after consent.')
        action = self.description['action']
        require(action in ('install', 'upgrade', 'uninstall', 'start'), 'Unknown installation action.')
        require((action == 'install') == (self.old is None),
                'The installed state changed. Review the operation again.')
        self.mkdir(self.root)
        # Keep the lock directory for the entire transaction; no racy unlink/relock window.
        self.lock = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Refused('Another daemon installation is in progress.')
        locked = self.preflight()
        require((action == 'install') == (self.old is None),
                'The installation changed while acquiring its lock.')
        require(locked['installation'] == initial['installation'],
                'The managed installation changed while acquiring its lock.')
        self.stage = Path(tempfile.mkdtemp(prefix='.ssh-stage-', dir=self.root))
        self.stage.chmod(0o700)
        self.previous_receipt = self.receipt.read_bytes() if self.receipt.exists() else None
        if action in ('install', 'upgrade'):
            artifact = self.description['artifact']
            require(artifact['architecture'] == locked['arch'], 'The Linux architecture changed.')
            require(0 < artifact['bytes'] <= 512 * 1024 ** 2, 'Invalid archive size.')
            if self.old:
                require(version(artifact['version']) > version(self.old['version']),
                        'Only a newer release may replace this managed daemon.')
            archive = self.stage / 'runtime.tar.gz'
            digest = hashlib.sha256()
            remaining = artifact['bytes']
            with archive.open('xb') as output:
                while remaining:
                    chunk = sys.stdin.buffer.read(min(65536, remaining))
                    require(bool(chunk), 'The archive upload was interrupted.')
                    digest.update(chunk)
                    output.write(chunk)
                    remaining -= len(chunk)
            require(digest.hexdigest() == artifact['sha256'], 'The uploaded archive checksum differs.')
            runtime = self.stage / 'runtime'
            runtime.mkdir(mode=0o700)
            safe_extract(archive, runtime)
            owned(runtime / 'bin/ketplus_daemon')
            owned(runtime / 'bin/ketplusctl')
            info = json.loads(run([str(runtime / 'bin/ketplus_daemon'), '--build-info'], self.env))
            require(info.get('product_version') == artifact['version'] and
                    info.get('build_revision') == artifact['revision'] and
                    info.get('control_url') == artifact['control_url'], 'The daemon build identity differs.')
            run([str(runtime / 'bin/ketplus_daemon'), '--check-runtime'], self.env)
            candidate = self.root / (artifact['version'] + '-' + artifact['revision'] + '-' + artifact['architecture'])
            require(not candidate.exists() and not candidate.is_symlink(),
                    'This runtime directory already exists; nothing was overwritten.')
            receipt = {'schema': 1, 'profile': self.profile, 'version': artifact['version'],
                       'inventory': inventory(runtime), 'directory': candidate.name,
                       'binaries': {name: file_hash(runtime / 'bin' / name)
                                    for name in ('ketplus_daemon', 'ketplusctl')}}
            encoded_receipt = json.dumps(receipt)
            require(len(encoded_receipt.encode()) <= 16 * 1024 ** 2, 'The runtime inventory is too large.')
            self.stop_old(artifact['version'])
            if self.data.exists():
                self.snapshot = self.stage / 'previous-data'
                shutil.copytree(self.data, self.snapshot, symlinks=True)
            runtime.rename(candidate)
            self.new = candidate
            link = self.stage / 'current'
            link.symlink_to(self.new.name)
            self.activated = True
            link.replace(self.current)
            self.child = self.start(self.new)
            self.receipt.write_text(encoded_receipt)
            self.receipt.chmod(0o600)
        elif action == 'start':
            require(not self.socket.exists(), 'The managed daemon is already running.')
            if self.data.exists():
                self.snapshot = self.stage / 'previous-data'
                shutil.copytree(self.data, self.snapshot, symlinks=True)
            self.activated = True
            self.child = self.start(self.previous)
        else:
            self.stop_old()
            self.activated = True
            self.current.unlink()
            self.receipt.unlink()
        emit({'phase': 'ready'})
        # Desktop acknowledges only after a real daemon.hello over the SSH tunnel. A dropped
        # connection/Cancel/timeout rolls back even if the local installer process is killed.
        require(bool(select.select([sys.stdin.buffer], [], [], 90)[0]), 'Connection confirmation timed out.')
        require(sys.stdin.buffer.readline(128) == b'commit\n', 'Installation was not confirmed.')
        self.committed = True
        if action == 'uninstall':
            shutil.rmtree(self.previous)
        emit({'phase': 'committed'})

    def cleanup(self):
        if not self.committed:
            if self.child and self.child.poll() is None:
                self.child.terminate()  # Only the process this transaction started.
                try:
                    self.child.wait(10)
                except subprocess.TimeoutExpired:
                    self.child.kill()
                    self.child.wait(5)
            if self.activated:
                if self.current.is_symlink():
                    self.current.unlink()
                if self.previous:
                    self.current.symlink_to(self.previous.name)
                if self.previous_receipt is None:
                    self.receipt.unlink(missing_ok=True)
                else:
                    self.receipt.write_bytes(self.previous_receipt)
                    self.receipt.chmod(0o600)
                if self.snapshot:
                    if self.data.exists():
                        shutil.rmtree(self.data)
                    self.snapshot.rename(self.data)
                elif self.description['action'] in ('install', 'start') and self.data in self.created:
                    shutil.rmtree(self.data, ignore_errors=True)
            if self.new and self.new.exists():
                shutil.rmtree(self.new)
            if self.created_token:
                self.token.unlink(missing_ok=True)
        if self.stage and self.stage.exists():
            shutil.rmtree(self.stage)
        if not self.committed and self.stopped and self.previous:
            restored = self.start(self.previous)
            for _ in range(100):
                require(restored.poll() is None, 'The previous daemon could not restart; check the host.')
                try:
                    if peer_pid(self.socket, .1) == restored.pid:
                        break
                except (OSError, Refused):
                    pass
                time.sleep(.1)
            else:
                raise Refused('The previous daemon restart could not be confirmed; check the host.')
        if self.lock is not None:
            os.close(self.lock)
        if not self.committed:
            for path in reversed(self.created):
                try:
                    path.rmdir()
                except OSError:
                    pass


def main():
    os.umask(0o077)
    def interrupted(_signal, _frame):
        raise Refused('The SSH install channel was interrupted.')
    signal.signal(signal.SIGHUP, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    transaction = None
    try:
        description = json.loads(base64.b64decode(sys.argv[1], validate=True))
        transaction = Transaction(description)
        os.chdir(transaction.home)
        if description['action'] == 'preflight':
            emit(transaction.preflight())
        else:
            transaction.activate()
    except (Refused, OSError, ValueError, KeyError, subprocess.SubprocessError,
            tarfile.TarError) as error:
        # Only our fixed refusal strings are safe to present. OSError/JSON/subprocess messages
        # may contain account paths or secrets and are deliberately not forwarded.
        emit({'error': str(error) if isinstance(error, Refused) else 'The installation operation failed.'})
        return 1
    finally:
        if transaction:
            transaction.cleanup()
            if description['action'] != 'preflight' and not transaction.committed:
                emit({'phase': 'rolled_back'})
    return 0


if __name__ == '__main__':
    sys.exit(main())
