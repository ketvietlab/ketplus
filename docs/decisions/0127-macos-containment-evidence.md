# 0127: macOS process containment evidence

Status: Evidence for the background-task design; not a support or implementation decision

Date: 2026-11-16

## Question and conclusion

Can a normally deployed macOS executor guarantee that every descendant stops
when its daemon or executor dies, including a double-forked process that calls
`setsid()`?

The mechanisms examined here do **not establish that guarantee for arbitrary
native commands**. On the tested host, a detached descendant survived both
`launchd` job termination and explicit process-group termination. A separate
supervisor, lease, PID inventory or restart reconciliation can improve recovery,
but requires a surviving cleanup actor and complete process membership. These
are not a kernel-enforced lifetime boundary.

This finding does not remove macOS support, change the background-task contract,
or authorize shipping a weaker executor. The leader must choose and validate a
containment design before T-2 can claim the required guarantee. It is not a proof
that every possible macOS mechanism is incapable of providing one.

## Mechanisms and evidence

### Process groups and launchd

Apple documents `setsid()` as creating a new session and process group. A child
that is not already a group leader can use it, including during double-fork
daemonization. `killpg()` targets a group, not a descendant tree. A PGID snapshot
therefore excludes descendants that leave that group. See Apple's
[setsid manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/setsid.2.html)
and the installed SDK's `man 2 killpg`.

`launchd.plist(5)` documents cleanup of remaining processes sharing the job's
PGID; `AbandonProcessGroup=true` disables it. The same rule appears in Apple's
[published launchd manual](https://github.com/apple-oss-distributions/launchd/blob/d448a1c8f70a61202f8705f94337f686b87c30c4/man/launchd.plist.5#L364).
The installed macOS 26.2 manual still states this behavior. `ExitTimeOut` bounds
the TERM-to-KILL grace for stopping a job; it does not expand membership.
The probes below exercise both executor crash and service removal.

Running a supervisor under `launchd` can keep it independent of the requesting
daemon. This is useful operational supervision, but its cleanup scope still
needs a separate containment argument. `KeepAlive` also needs care: relaunching
an executor must not replay an already accepted command. Apple's
[launchd guide](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)
explains per-user service lifecycle and relaunch behavior. Killing the system's
`launchd` itself was neither attempted nor needed; the crash probe kills only a
new, uniquely named test service's executable.

### Identity and crash recovery

Checking PID plus start time before `kill(pid)` leaves a check/use race. A process
can exit between lookup and signal; persisted PID/PGID values must not authorize
signalling a later occupant. A durable registry records intent and recovery
state, but cannot by itself prevent process escape or recover an unregistered
descendant after the only observer crashes.

The installed SDK exposes `proc_signal_with_audittoken`. Apple's published XNU
[libproc wrapper](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/libsyscall/wrappers/libproc/libproc.c#L487)
uses a token-based syscall. Kernel
[process lookup](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_proc.c#L445)
validates the token's PID version. The local identity probe below confirms
rejection of a deliberately mismatched version. This is evidence for individual
process identity, not complete membership, API availability on older deployment
targets, or a tested real PID-recycling scenario. Receiving a trustworthy token
for every descendant, including crash windows, remains a separate problem.

### Sandbox, coalitions and event monitoring

Apple's [App Sandbox](https://developer.apple.com/documentation/security/app-sandbox/)
restricts resource access. Its documented
[inheritance entitlement](https://developer.apple.com/library/archive/documentation/Miscellaneous/Reference/EntitlementKeyReference/Chapters/EnablingAppSandbox.html)
passes sandbox restrictions to child helpers; that is not a documented
kill-all-descendants-on-parent-death contract. Requiring appropriately signed
helpers and restricting file/network access would also affect arbitrary build
tools. The installed `sandbox-exec(1)` manual marks that tool deprecated. No
custom Seatbelt policy, syscall interception, or sandbox lifecycle guarantee was
validated here; merely denying all forks would change the required workload.

XNU has kernel coalitions, but its
[termination-request implementation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/coalition.c#L2179)
marks a request and notifies when the active count reaches zero; its name is not
evidence of a public kill-tree API. Source also distinguishes privileged
coalition management. No supported per-job coalition interface or crash cleanup
contract for an ordinary distributed app was established. This source snapshot
is explanatory evidence, not a claim that its commit identifies the running OS.

Endpoint Security can supply process events, but an event consumer still needs
a proven enforcement and failure model. Apple's
[client entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.endpoint-security.client)
requires approval from Apple. The normal
[client API](https://developer.apple.com/documentation/endpointsecurity/es_new_client(_:_:))
also documents user TCC approval. A newer
[descendants client](https://developer.apple.com/documentation/endpointsecurity/es_new_descendants_client(_:_:).md)
is documented for macOS 27.0+, without root or TCC, but still with the restricted
entitlement. It describes subtree event visibility, not automatic tree
termination when the client dies. It was absent from this host's 26.5 SDK headers and
was not run. Neither variant is an entitlement-free fix for existing targets.

### Virtual-machine boundary

An isolated VM is a candidate containment boundary for guest processes. Apple's
[VM stop API](https://developer.apple.com/documentation/virtualization/vzvirtualmachine/stop(completionhandler:).md)
can stop a VM without guest cooperation; the framework requires the
[virtualization entitlement](https://developer.apple.com/documentation/virtualization/adding-the-virtualization-entitlement-to-your-project).
This is a direction to investigate, not a verified implementation: executor-host
crash, daemon reconnect, guest/host bridges and resource teardown need probes.
Host-side command execution delegated by a guest would lie outside that boundary.
Guest OS/toolchain compatibility, shared directories, image licensing, storage,
signing and deployment changes need a leader decision. No VM, entitlement,
system extension, installed app or launch configuration was deployed by T-6.

## Local probes

Environment: macOS 26.2, build 25C56, arm64; Command Line Tools SDK 26.5.
Only synthetic Python workers and a
small C identity probe ran. No provider was invoked and no existing daemon,
service or conversation was signalled. The Python harness used a private
temporary directory inside this task's checkout and a random service label in
the current user's GUI domain; it installed no login item or persistent plist.

Each worker held a private control connection, answered a ping and self-exited
after at most 15 seconds, on control EOF, or on a cleanup request. The detached
worker used `fork -> setsid -> fork`, and its intermediate parent exited and was
reaped. Thus the final worker had a different PGID/session from the root and was
reparented to PID 1. No cleanup signal used a discovered or persisted PID.

| Trigger | Root | Ordinary same-group child | Detached grandchild |
| --- | --- | --- | --- |
| `killpg(root_pgid, SIGKILL)` | Exited | Exited | Answered ping |
| `SIGKILL` of ordinary supervisor only | Exited | Answered ping | Answered ping |
| `launchctl kill SIGKILL <test-service>` | Exited | Exited | Answered ping |
| `launchctl bootout <test-service>` | Exited | Exited | Answered ping |

Both launchd cases explicitly set `AbandonProcessGroup=false`,
`KeepAlive=false`, `RunAtLoad=true`, and `ExitTimeOut=2`. The observations prove a
survivor for these cases; they are not a timing guarantee for every macOS version
or an exhaustive fork/signal stress test.

The following reproduces the four cases. Run it as a logged-in macOS user from
a disposable checkout with Python 3.11+. The harness removes only its own random
service, verifies it is absent, closes worker channels, reaps its direct root,
and checks the recorded probe PIDs have disappeared. An assertion failure is a
probe failure, not permission to kill other processes or change system policy.

```python
import json, os, pathlib, plistlib, signal, socket, subprocess, sys
import tempfile, time, uuid

worker = r'''
import json, os, socket, sys, time
def live(role):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(15)
    s.connect(sys.argv[1])
    s.sendall((json.dumps(dict(role=role, pid=os.getpid(),
        pgid=os.getpgrp(), sid=os.getsid(0)))+'\n').encode())
    end = time.monotonic()+15
    while time.monotonic() < end:
        s.settimeout(max(.01, end-time.monotonic()))
        try: data = s.recv(1)
        except TimeoutError: break
        if not data or data == b'q': break
        s.sendall(b'y')
    s.close()
    os._exit(0)
if os.fork() == 0: live('ordinary')
p = os.fork()
if p == 0:
    os.setsid()
    if os.fork() != 0: os._exit(0)
    live('detached')
os.waitpid(p, 0)
live('root')
'''

def call(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=6)

def alive(s):
    try:
        s.sendall(b'p')
        s.settimeout(.5)
        return s.recv(1) == b'y'
    except OSError: return False

for mode in ['killpg', 'supervisor_crash', 'launchd_crash', 'launchd_bootout']:
    conns, records = {}, {}
    proc, target, loaded = None, None, False
    with tempfile.TemporaryDirectory(prefix='.t6-', dir='.') as tmp:
        base = pathlib.Path(tmp).resolve()
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(base/'s'))
        server.listen()
        server.settimeout(5)
        try:
            args = [sys.executable, '-c', worker, str(base/'s')]
            if mode.startswith('launchd'):
                label = 'org.ketplus.containment-probe.'+uuid.uuid4().hex
                domain = 'gui/'+str(os.getuid())
                target = domain+'/'+label
                plist = base/'job.plist'
                plist.write_bytes(plistlib.dumps(dict(Label=label,
                    ProgramArguments=args, RunAtLoad=True, KeepAlive=False,
                    AbandonProcessGroup=False, ExitTimeOut=2)))
                result = call(['/bin/launchctl', 'bootstrap', domain, str(plist)])
                assert result.returncode == 0, result.stderr
                loaded = True
            else:
                proc = subprocess.Popen(args, start_new_session=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(3):
                s, _ = server.accept()
                s.settimeout(2)
                line = b''
                while not line.endswith(b'\n'):
                    chunk = s.recv(1)
                    assert chunk, 'worker exited before identification'
                    line += chunk
                record = json.loads(line)
                conns[record['role']], records[record['role']] = s, record
            assert records['ordinary']['pgid'] == records['root']['pgid']
            assert records['detached']['pgid'] != records['root']['pgid']
            assert records['detached']['sid'] != records['root']['sid']
            if mode == 'killpg': os.killpg(proc.pid, signal.SIGKILL)
            elif mode == 'supervisor_crash': proc.kill()
            else:
                command = (['kill', 'SIGKILL', target] if mode == 'launchd_crash'
                           else ['bootout', target])
                result = call(['/bin/launchctl']+command)
                assert result.returncode == 0, result.stderr
            time.sleep(.4)
            result = {role: alive(s) for role, s in conns.items()}
            print(mode, json.dumps(result), flush=True)
            assert result == dict(root=False, ordinary=mode == 'supervisor_crash',
                                  detached=True)
        finally:
            if loaded:
                call(['/bin/launchctl', 'bootout', target])
                assert call(['/bin/launchctl', 'print', target]).returncode != 0
            for s in conns.values():
                try: s.sendall(b'q')
                except OSError: pass
            for s in conns.values():
                s.settimeout(16)
                try:
                    while s.recv(1024): pass
                except ConnectionResetError: pass
                s.close()
            if proc is not None: proc.wait(timeout=16)
            server.close()
            if records:
                pids = ','.join(str(r['pid']) for r in records.values())
                for _ in range(40):
                    remaining = call(['/bin/ps', '-p', pids, '-o', 'pid=']).stdout
                    if not remaining.strip(): break
                    time.sleep(.05)
                assert not remaining.strip(), 'probe PID still present; inspect'
            print(mode, 'cleanup verified', flush=True)
```

### PID-version probe

A separate temporary C executable obtained its own `TASK_AUDIT_TOKEN` using
`task_info`, installed a `SIGUSR1` counter, and called
`proc_signal_with_audittoken(&token, SIGUSR1)`. It then toggled `token.val[7]` and
repeated the call. Compiled with `xcrun clang -Wall -Wextra -Werror -lproc`:

```c
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <mach/mach.h>
#include <libproc.h>
static volatile sig_atomic_t received = 0;
static void handler(int sig) { (void)sig; ++received; }
int main(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN,
                  (task_info_t)&token, &count) != KERN_SUCCESS) return 2;
    signal(SIGUSR1, handler);
    int valid = proc_signal_with_audittoken(&token, SIGUSR1);
    for (int i = 0; i < 100 && received == 0; ++i) usleep(1000);
    token.val[7] ^= 1;
    int stale = proc_signal_with_audittoken(&token, SIGUSR1);
    usleep(10000);
    printf("audit-token SIGUSR1: valid=%d changed-pidversion=%d ESRCH=%d received=%d\n",
           valid, stale, ESRCH, received);
    return valid == 0 && stale == ESRCH && received == 1 ? 0 : 1;
}
```

Observed output:

```text
audit-token SIGUSR1: valid=0 changed-pidversion=3 ESRCH=3 received=1
```

An initial attempt using signal 0 returned `EINVAL` (22) for both tokens on this
host; this interface must not be assumed to support `kill(pid, 0)` semantics.
The corrected probe sent only a handled signal to itself. It did not force PID
reuse, signal another app, or test remote acquisition/persistence of audit tokens.
Temporary binaries and directories were removed.

## Recommendation to the leader

Keep the durable registry, authenticated executor handshake, bounded leases and
safe identity checks as recovery requirements, but do not use them as evidence
that native descendants cannot escape. In particular, do not acknowledge
`cancelled` or `timed_out` merely because the supervisor/group disappeared while
membership outside that group is unknown.

Choose a concrete additional boundary and require adversarial crash probes for
it: for example, investigate an isolated VM, or obtain and validate a supported
native enforcement mechanism with an explicit workload/deployment contract.
Neither has been selected by this evidence note. A restricted/cooperative
workload could make supervision tractable, but that changes the guarantee and
requires an explicit design decision; T-6 does not make that change.

Before claiming completion, cover fork before registration, group/session escape,
executor and daemon crash independently, cleanup actor crash, stop/complete race,
PID-version mismatch, timeout escalation, reboot/reconnect, and process creation
during teardown. Include the oldest supported macOS target and supported
architectures. Probes here cover only the rows above and the identity check;
they do not replace T-2's runtime/recovery suite. No product build, deployment,
UI acceptance or provider-quota test is claimed.
