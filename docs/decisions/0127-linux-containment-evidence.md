# 0127: Linux daemon-user containment evidence

Status: environment and primitive evidence; not production executor acceptance

Date: 2026-11-16

## Result and limits

A disposable Ubuntu 24.04.4 ARM64 guest can supply a real **unprivileged
daemon-user** route to delegated cgroup v2 domains and user/cgroup/mount/PID
namespaces. The initial image could not: it lacked a user bus, and AppArmor
blocked the user-namespace UID mapping. Guest-only configuration resolved those
two blockers. No macOS privilege configuration or product broker was installed.

The probes establish these conditional building blocks:

- A trusted launcher can wait behind a pipe barrier, be placed in its job
  domain before workload exec, and exit without running the workload when its
  supervisor dies before release.
- With the pre-opened migration FD closed, a workload rooted in the job's
  cgroup namespace could not migrate to a sibling through ordinary paths,
  `/proc` aliases, a deliberately inherited root-directory alias, a new cgroup
  mount, or an ancestor namespace handle.
- `cgroup.kill` followed by `populated=0` reclaimed ordinary children and
  double-forked/`setsid()` descendants. Killing namespace PID 1 through a
  live pidfd also reclaimed that namespace's descendants.
- A synthetic daemon's SIGKILL closed its independent supervisor's lease pipe;
  the supervisor killed the domain and confirmed it empty. A supervisor that
  is itself the MainPID of a **user** service was reclaimed with its descendants
  by the existing user manager after MainPID SIGKILL.

Three negative controls matter just as much:

1. Delegation to the workload's UID without a cgroup namespace allowed migration.
2. A writable `cgroup.procs` FD opened **before** entering the namespace allowed
   migration even inside the namespace. Closing unrelated/authority-bearing
   descriptors before workload exec is necessary; hiding paths is insufficient.
3. An external same-UID writer in the original namespace could move the tested
   processes to a sibling. The namespace limits the workload's own migration
   authority; it does not revoke an external actor's delegated authority.

An unsupervised supervisor's death also left its job populated until the test
oracle reclaimed it. A delegated cgroup alone is not an automatic lifetime
boundary. The positive user-service test depends on a surviving user manager,
`KillMode=control-group`, and a finite stop deadline. This dependency must be
part of any proposed deployment, rather than silently assumed.

This is **not** proof of T-2's durable registry, launch-intent transaction,
boot/unit identity verification, restart reconciliation, concurrent Stop,
output handling, or permission enforcement. The synthetic barrier has no
production journal. No production daemon/helper was built or invoked. The
leader still owns the Linux executor/deployment decision under
[ADR 0127](0127-daemon-background-tasks.md); macOS best effort is not applied here.
Admission must remain closed unless the chosen implementation establishes all
required conditions. In particular, these results must not be advertised as
protection against every independently privileged same-UID actor or deliberate
requests to external services.

## Environment and ownership

The test used an exclusively handed-off Colima VZ profile with 2 CPUs, 2 GiB RAM,
an 8 GiB guest disk and the containerd runtime. Colima was already available on
the host. The earlier owner's VM had been deleted before handoff; T-7 recreated
only that dedicated profile. No other VM was started, stopped or changed.

| Capability | Initial guest | Configured guest |
| --- | --- | --- |
| OS/kernel | Ubuntu 24.04.4 LTS / 6.8.0-117-generic | Same |
| Architecture/systemd | aarch64 / 255.4-1ubuntu8.15 | Same |
| Real daemon UID | 501 | 501, including transient user-service supervisor |
| cgroup v2 | `rw,nsdelegate,memory_recursiveprot` | Same; delegated domain under user manager |
| `kernel.unprivileged_userns_clone` | 1 | 1 |
| `kernel.apparmor_restrict_unprivileged_userns` | 1 | 0, runtime guest-only change |
| User namespace/PID/mount/cgroup unshare | `uid_map` write failed with EPERM | Succeeded |
| User bus | Missing | `dbus-user-session` installed; user manager and `dbus.socket` started |

The initial kernel audit recorded `userns_create` transitioning `unconfined`
to `unprivileged_userns`, followed by AppArmor denial of `CAP_SYS_ADMIN`.
Changing only the guest restriction made the same unshare command succeed.
This distinguishes the cause from the already enabled kernel userns sysctl.

UID 0 reported **inside** the workload's user namespace maps to the real
unprivileged UID 501; it is not guest-root execution. Guest administration used
`sudo` only for test setup. The workload and service probes never used
`sudo systemd-run`. A prior root system-service probe is not counted here.

## Reproduce in a disposable guest

Do not apply the guest-administration commands to a development workstation or
production server. The broad AppArmor switch below is a test capability change,
not a recommendation to disable that restriction in deployment. A production
administrator would need a reviewed namespace policy for the actual launcher.

Create an exclusively owned test profile on the host, reusing an existing
profile only after its owner's explicit handoff:

```sh
colima start --profile ketplus-background-t2 --cpu 2 --memory 2 --disk 8 \
  --runtime containerd --vm-type vz
colima ssh --profile ketplus-background-t2
```

Inside that guest, record the initial state and perform the guest-only setup:

```sh
id
uname -r
systemd --version
findmnt -n -t cgroup2 -o TARGET,OPTIONS
sysctl kernel.unprivileged_userns_clone \
  kernel.apparmor_restrict_unprivileged_userns user.max_user_namespaces
unshare -UrmpfC --mount-proc true
# The fresh test image fails this command with uid_map EPERM.
sudo dmesg | tail -50 | grep -E 'apparmor|userns|DENIED'

sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
unshare -UrmpfC --mount-proc true
sudo apt-get update -qq
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y dbus-user-session
probe_uid=$(id -u)
sudo loginctl enable-linger "$(id -un)"
sudo systemctl start "user@$probe_uid.service"
export XDG_RUNTIME_DIR="/run/user/$probe_uid"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
systemctl --user daemon-reload
systemctl --user start dbus.socket
test -S "$XDG_RUNTIME_DIR/bus"
```

Copy the two Python files from `tests/daemon/linux-containment/` into a private
guest directory. For a Colima-mounted checkout, use the guest-visible checkout
path as `REPO` below. The path must be absolute; Colima flattens remote command
arguments, so a saved guest shell script is preferable to nested `sh -c` quoting.

```sh
# REPO is the absolute path of this checkout as visible inside this guest.
mkdir -p "$HOME/ketplus-t7-probes"
cp "$REPO"/tests/daemon/linux-containment/{probe,user_unit}.py "$HOME/ketplus-t7-probes/"
cd "$HOME/ketplus-t7-probes"
systemd-run --user --wait --collect --pipe --unit=ketplus-t7-containment \
  -p Delegate=yes python3 "$PWD/probe.py"
python3 "$PWD/user_unit.py"
```

`probe.py` runs nine cases in its own disposable delegated user service.
`user_unit.py` creates a unique additional user service with `Delegate=yes`,
`KillMode=control-group`, `TimeoutStopSec=1s`, `RuntimeMaxSec=15s` and `Restart=no`.
Its MainPID is the synthetic supervisor outside the per-job child cgroup.
It sends SIGKILL through the manager's unit identity, then requires removal
of the unit cgroup, which cannot be removed while populated. Neither test
uses a saved PID as signal authority. The namespace-init test opens a live
pidfd and checks containment before signalling it.

These are opt-in Linux tests. They reject guest-root driver execution and do
not belong in the normal macOS/offscreen or provider suites. Every synthetic
worker has a 25-second self-expiry; each test also kills its owned job and
escape-control domains in cleanup. No provider account, model, runtime build,
privileged product broker or new dependency in KetPlus is required.

## Recorded observations

The machine-readable [evidence](../evidence/linux-containment-2026-11-16.json)
contains normalized observed output and SHA-256 hashes of the exact test files.
Machine paths, random unit IDs, host identities and raw process IDs are omitted.

| Case | Observation |
| --- | --- |
| Delegation only | Path, peer-root, root-directory alias and inherited FD migrations succeeded; sibling remained populated after job kill |
| Descriptor leak | Namespace path/alias/remount migrations failed ENOENT, ancestor `setns` failed EPERM, but inherited cgroup FD migrated successfully |
| Barrier supervisor crash before release | No payload marker, job became `populated=0` without running payload |
| Job kill | Ordinary child plus detached grandchild reclaimed; job `populated=0`, sibling empty |
| Namespace init death | Live-pidfd SIGKILL of PID 1 reclaimed namespace descendants; job empty |
| Lease EOF | Supervisor confirmed job empty |
| Synthetic daemon SIGKILL | Lease EOF triggered supervisor cleanup and explicit reclamation acknowledgement |
| Bare supervisor SIGKILL | Job stayed populated until oracle cleanup; negative control |
| External same-UID peer | Four migrations allowed; sibling remained populated after job kill; oracle reclaimed it |
| User-service supervisor SIGKILL | Existing user manager reclaimed descendants and removed its unit domain |

All ten cases completed with their expected positive or negative outcomes.
Expected negative controls are recorded limitations, not successful containment.
The scripts fail if a purportedly blocked workload migration succeeds or a
required positive cleanup does not finish. The external-peer and leaked-FD
controls deliberately demonstrate the opposite and require the escape.

## Deployment requirements and unverified behavior

The supplied KetPlus Linux user unit does not yet declare the per-job user-unit
and namespace arrangement tested here. Having a normal daemon user service is
not equivalent to the tested executor supervisor being that job unit's MainPID.
The initial image's lack of a user bus also shows why the daemon must check
capabilities instead of assuming its login environment exists.

Any implementation consuming this environment needs a delegated domain with
`cgroup.kill`/`populated`, usable unprivileged namespaces, descriptor hygiene,
an independent lease, and a tested surviving cleanup actor for supervisor loss.
The mount, cgroup namespace, UID mapping and proc view must be established
before arbitrary workload exec. This fixture intentionally retains read-only
root/namespace handles to challenge isolation; that is not a launcher policy.

These probes do not isolate network, workspace permissions, DBus calls or all
host filesystem shares. Independently running same-UID actors, user-manager
death/restart, guest reboot, real PID recycling, cgroup inode reuse, launch
intent crashes and recovery from production metadata remain unverified here.
The tests exercise systemd 255 and kernel 6.8 on ARM64 only. Other kernels,
architectures, AppArmor policies or systemd versions need their own verification.

The kernel documents [delegation and migration containment](https://docs.kernel.org/admin-guide/cgroup-v2.html#delegation-containment)
and [cgroup.kill](https://docs.kernel.org/admin-guide/cgroup-v2.html#core-interface-files).
Its [PID namespace documentation](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)
describes namespace-init death. systemd documents the tested
[unit cleanup policy](https://www.freedesktop.org/software/systemd/man/latest/systemd.kill.html)
and [delegation](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html).
Current [execution isolation documentation](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)
explicitly does not support `ProtectControlGroups=` or `ProtectProc=` for
per-user services; no claim here relies on those settings working in this route.

## Cleanup and handoff

The probes reclaim their own job/sibling domains, stop their unique transient
units, remove temporary worker files, and do not restart any command. Confirm
there are no remaining `ketplus-t7-*` units/workloads before handing off the
profile. The guest-only sysctl setting is not persisted across reboot; the
installed package and lingering setting remain until the guest is deleted.
The final task handoff records whether the exclusively owned test profile is
retained for T-2 or deleted. That operational handoff is not product deployment.

At the final T-7 handoff, the profile was retained running for the T-2 developer,
who became its sole mutation owner. A final rerun from the private guest copy
completed, with zero remaining test transient units and zero live probe
workloads; user bus and unshare capability checks still succeeded. The guest
copy at `~/ketplus-t7-probes` contains both scripts, normalized evidence and logs,
so removal of T-7's host worktree does not break the recipient's fixtures.
One final injected-kill run printed an `unshare` signal-mask diagnostic;
the assertions and final empty-workload checks still passed. It is not counted
as a new capability or hidden as an executor result.

To discard an exclusively owned profile after its recipient is finished:

```sh
colima delete --profile ketplus-background-t2 --force --data
```

Do not run that command after another developer has taken ownership without a
new handoff.
