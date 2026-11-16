# Daemon identity and agent permissions audit — T-1

Review date: 2026-11-16. Public baseline: `7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`
(the audit branch and `integration` HEAD when the task was received).

**Recommendation: hold release pending reviewer verification of DA-01 and DA-02.**
Source analysis identified two High-severity findings and one Informational
documentation discrepancy. This investigation does not establish exploitation,
provide a fix, or certify the security of the entire product. T-1 changes no
product code.

## Scope and trust model

Reviewed `AGENTS.md` and the relevant sections of `project-status.md`,
`repository-boundaries.md`, `architecture.md`, `daemon-architecture.md`,
`agent-workspace.md`, `development.md`, `testing.md`, `product-architecture.md`,
`licensing.md`, ADR 0034 on device pins, and ADR 0036 on challenge v2.

- Reviewed only public code in `apps/daemon`, `modules/remote`, `modules/ai`, and
  related fixtures. No real credential stores, transcripts, or account settings
  were read.
- Under ADR 0034, the relay may be actively malicious; its device list does not
  independently grant authority. Pins approved by the user on this machine are
  the trust anchor, except for the explicitly documented one-time migration of
  existing enrollments.
- The local control token grants OS-user-level authority. An agent MCP token
  grants agent operations under a conversation/generation identity. This is not
  a sandbox preventing all processes under the same OS user from reading each
  other's data.
- File/shell permissions depend on the adapter and provider sandbox. UI profile
  names do not prove equivalent isolation across providers. Injected browser and
  computer tools have separate policies; exemption from per-call prompts is not
  inherently a vulnerability.
- Hosted authorization, tenant membership, one-time challenge redemption, and
  service deployment are outside the evidence available in this repository.

## Reviewed coverage

The following entry points were traced; this does not imply every file in each
module was reviewed. Line references and conclusions apply to the baseline above.

| Boundary | Code evidence | Result / limitation |
| --- | --- | --- |
| Local IPC admission | [LocalIpcServer.cpp:50](../../apps/daemon/src/ipc/LocalIpcServer.cpp#L50), `:241–355`, `:584–593`; [DaemonCredentialFile.cpp:16](../../modules/remote/src/DaemonCredentialFile.cpp#L16) | Socket `UserAccessOption`, mandatory hello, and full-byte control-token comparison; agent tokens take a separate path. Unix token files use no-follow/nonblocking open, descriptor-based owner/type/mode checks, and a 4-KiB bound. Windows ACLs were not verified in this review. |
| Agent identity and scope | [AgentBroker.cpp:136](../../apps/daemon/src/agents/AgentBroker.cpp#L136), `:155–194`; [SessionHostAgents.cpp:43](../../apps/daemon/src/runtime/SessionHostAgents.cpp#L43); [AgentMcpServer.cpp:389](../../apps/daemon/src/agents/AgentMcpServer.cpp#L389) | Random tokens map to session/generation; IPC permits only agent operations, and the broker checks the current generation before dispatch. Team tools also pass through a separate role layer; the complete role matrix/team workflow was not audited. |
| Approvals and conversation isolation | [SessionHost.cpp:2001](../../apps/daemon/src/runtime/SessionHost.cpp#L2001), `:2064–2078`; [ApprovalBroker.cpp:202](../../apps/daemon/src/runtime/ApprovalBroker.cpp#L202); [AcpPermissions.cpp:12](../../apps/daemon/src/providers/acp/AcpPermissions.cpp#L12), `:57–78` | Approvals bind session/generation/kind and reject a second resolution. ACP checks the current turn and selects only `allow_once`/`reject_once`, without promotion to an always rule. Timer expiry was reviewed in code; Qt fixtures were not run. |
| UI client events / permission changes | [CodexAppServerDaemonEvents.cpp:32](../../modules/ai/src/agent/CodexAppServerDaemonEvents.cpp#L32), `:95–108`; [AgentSessionConfiguration.cpp:63](../../modules/ai/src/agent/AgentSessionConfiguration.cpp#L63) | The client routes by session and drops mismatched generations/old cursors; changing a model does not automatically write permissions. UI checks do not replace daemon checks. Not every UI callback was audited. |
| Provider policy | [SessionHostTurns.cpp:33](../../apps/daemon/src/runtime/SessionHostTurns.cpp#L33); [ClaudeAdapter.cpp:238](../../apps/daemon/src/providers/ClaudeAdapter.cpp#L238); [CursorAdapter.cpp:255](../../apps/daemon/src/providers/CursorAdapter.cpp#L255); [review-guard.mjs](../../apps/daemon/providers/claude-sidecar/review-guard.mjs) | Codex maps sandbox and approval policy; Claude maps plan/acceptEdits/bypass; Cursor maps ask/agent. Six fake-SDK tests checked the sidecar read/verify review profiles. This does not establish sandbox enforcement by the real SDK or external CLIs. |
| Browser sign-in / challenge | [BrowserSignIn.cpp:58](../../modules/remote/src/cloud/BrowserSignIn.cpp#L58), `:94–133`; [CloudControlSignIn.cpp:33](../../apps/daemon/src/cloud/CloudControlSignIn.cpp#L33), `:124–235`; [CloudControlCredentials.cpp:25](../../apps/daemon/src/cloud/CloudControlCredentials.cpp#L25) | Loopback/state, PKCE handoff, same-origin validation of the returned page, and sign-in/challenge epochs are present. The client requires challenge v2; correct service-side one-time redemption is not established. |
| Remote identity / sign-out | [DevicePins.cpp:69](../../apps/daemon/src/cloud/DevicePins.cpp#L69); [CloudControlClient.cpp:367](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367), `:620–770`; [CloudControlSync.cpp:447](../../apps/daemon/src/cloud/CloudControlSync.cpp#L447); [CloudControlCommands.cpp:249](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L249) | Digest validation precedes pairing and key pinning; DA-01/02 concern callback/cache lifecycle. Heartbeat epoch guards do not imply equivalent guards in every callback. |
| Remote client / peer grant | [ControlV2Authentication.cpp:30](../../modules/remote/src/control/ControlV2Authentication.cpp#L30), `:229–267`; [RemotePeers.cpp:76](../../apps/daemon/src/transport/RemotePeers.cpp#L76) | Desktop credential changes/sign-out advance the epoch, clear the signer cache, and empty host pins. Peers check grant, capability, machine/workspace, and device; the complete WebRTC stack was not audited. |
| Keys and storage | [CloudCrypto.cpp:537](../../modules/remote/src/cloud/CloudCrypto.cpp#L537), `:661–675`; [SessionCredentials.cpp:78](../../apps/daemon/src/cloud/SessionCredentials.cpp#L78); [CredentialStore.cpp:22](../../modules/remote/src/cloud/CredentialStore.cpp#L22), `:94–146`, `:185–205` | Wrapping uses X25519/HKDF/AES-GCM and signatures; workspace changes set content keys aside separately. Credentials actually reside in an owner-only file rather than the OS keychain (DA-03). No independent cryptographic assessment was performed. |

## DA-01 — A key-distribution callback survives sign-out and reopens adoption

**Severity: High — confidentiality; violation of the untrusted-relay boundary.**
Status: control/data flow confirmed through source analysis; no C++ runtime
reproduction was performed.

**Expected:** After sign-out, replies originating in an earlier epoch must not
write pins, create/distribute content keys, or restore trust. Adoption is reserved
for actual legacy enrollments, not a pin set just removed by sign-out.

**Actual behavior and evidence chain:**

1. [CloudControlSync.cpp:454–465](../../apps/daemon/src/cloud/CloudControlSync.cpp#L454)
   checks `machineId_`/`daemonToken_` only before sending the request. The callback
   captures `this`, without capturing/comparing `accountEpoch_`, machine, or
   workspace.
2. [CloudControlClient.cpp:367–410](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367)
   advances the epoch, removes enrollment and `device-pins`, and clears the
   machine/token. Content keys and the identity signing key are not in the removal
   set. [request:272–332](../../apps/daemon/src/cloud/CloudControlClient.cpp#L272)
   still invokes the reply callback; sign-out does not cancel all in-flight requests.
3. When the device-list reply arrives after sign-out, `trustedDevices()` calls
   `pins_.adopt()`
   ([CloudControlSync.cpp:447–451](../../apps/daemon/src/cloud/CloudControlSync.cpp#L447)).
   [DevicePins.cpp:69–94](../../apps/daemon/src/cloud/DevicePins.cpp#L69) treats the
   absent pin set as uninitialized and persists active devices with valid public
   keys from the list as trusted. No enrollment/epoch check occurs here.
4. The callback continues through the journal, wraps keys for sessions that have
   not been refused, writes the cache, and POSTs envelopes
   ([CloudControlSync.cpp:469–519](../../apps/daemon/src/cloud/CloudControlSync.cpp#L469)).
   `wrapSessionKey()` still obtains the content key and signs;
   `validWrappedSessionKey()` does not require a nonempty `senderDeviceId`
   ([CloudCrypto.cpp:569–610](../../modules/remote/src/cloud/CloudCrypto.cpp#L569),
   [:661–675](../../modules/remote/src/cloud/CloudCrypto.cpp#L661)).

**Prerequisites/impact:** The daemon remains alive with a session/content key and
a pending device-list request; sign-out succeeds before the reply, and the
credential store remains writable. A malicious relay within the threat model can
return public keys it controls. The daemon then creates wraps for an unapproved
device and sends the body to the relay. The post-sign-out POST may lack a bearer;
rejection by an honest service does not protect the body from a malicious relay
that has already received it. The 500-envelope limit per invocation still applies.
No subsequent sign-in is required or assumed for this key-disclosure path.

**Remediation / acceptance for a separate fix:** Fence the callback before every
mutation using the captured epoch and machine/workspace; reject it when no longer
enrolled. Make adoption depend on explicit migration state rather than merely
missing pins. Apply the same rule to pairing approval/revocation callbacks and
fallback sender-key adoption. Add a fake-transport fixture that holds the reply,
signs out, then returns a synthetic device list: assert no new pin, wrap, device
refresh, or key-upload request. Also cover sign-out → different enrollment → old
reply. No real provider or network is needed.

## DA-02 — The sender-key cache bypasses pin reset after signing in again

**Severity: High — authorization; requires a previously trusted device.**
Status: missing cache invalidation confirmed through source analysis; end-to-end
command execution was not reproduced.

**Expected:** A new enrollment starts with an empty pin set; a key accepted before
sign-out no longer proves the device is paired with the new enrollment.

**Actual behavior and evidence:**

- [CloudControlCommands.cpp:249–258](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L249)
  returns cache hits immediately, before `pins_.trustsKeys()`. The cache key is
  only device/version; its TTL is 10 minutes
  ([CloudControlClient.h:62](../../apps/daemon/src/cloud/CloudControlClient.h#L62)).
- [Sign-out:367–410](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367) does
  not clear `senderKeys_`.
  [Enrollment:198–234](../../apps/daemon/src/cloud/CloudControlSignIn.cpp#L198)
  resets pins but neither clears nor changes the cache namespace. Cache hits do
  not consider `accountEpoch_`. Device revocation, by comparison, does clear the
  cache ([CloudControlClient.cpp:756](../../apps/daemon/src/cloud/CloudControlClient.cpp#L756)).
- [executeCommand:301–366](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L301)
  uses the key from `withSenderKeys` for signature verification/decryption;
  the cache-hit path does not recheck the pin. Peer commands have an additional
  grant check; this finding concerns the relay mailbox path. Staying in the same
  workspace preserves content keys
  ([SessionCredentials.cpp:78–91](../../apps/daemon/src/cloud/SessionCredentials.cpp#L78)).

**Prerequisites/impact:** Within the same process, device D was paired and warmed
the cache; the user signs out and enrolls again in the same workspace before the
TTL expires. The new pin set is empty, but the authentication function still
accepts D's key. D, or a party holding D's private key, can sign a suitable command;
a malicious relay can deliver it with the current scope. This creates a window
in which a reset identity remains accepted. It does not permit forging an
arbitrary device's signature or establish a provider sandbox escape. Machine,
generation, ciphertext, command-kind, and idempotency checks remain in place;
individual commands must satisfy them. Reuse of old ciphertext/content keys
across different workspaces is not claimed.

**Remediation / acceptance for a separate fix:** Clear the cache on every account
transition, fence asynchronous lookups by epoch, and recheck pins on cache hits;
the key cache must not replace current authorization. Fake fixture: cache D →
sign-out → enroll in the same workspace → lookup D must be rejected until a new
pairing is approved. Add delayed lookup replies across sign-out and revocation;
retain the normal cache test that avoids unnecessary network requests.

## DA-03 — The trust-boundary document promises an OS credential store; code uses a file

**Severity: Informational — storage-contract discrepancy; no demonstrated bypass.**

[daemon-architecture.md:677–679](../daemon-architecture.md#L677) requires long-lived
device keys in the OS credential store, never plaintext configuration.
[CredentialStore.cpp:22–28](../../modules/remote/src/cloud/CredentialStore.cpp#L22)
deliberately selects a file because of code-signing/keychain prompts;
[:185–205](../../modules/remote/src/cloud/CredentialStore.cpp#L185) serializes
base64 values into JSON. Base64 is not encryption. `save()` sets file mode 0600
and directory mode 0700 on Unix; these are useful controls but do not fulfill the
keychain promise.

Impact: maintainers/users may incorrectly assess at-rest protection, backups,
and processes running as the same OS user. This does not establish access by a
different OS user, and this store should not be conflated with the IPC token's
stricter descriptor admission. The leader should settle the key-storage model,
then update documentation/ADR or open a task to implement the appropriate store;
verify ACLs/backups separately on each platform. T-1 does not change storage policy.

## Checks, existing evidence, and limitations

Executed offline in the baseline worktree:

```sh
node --test apps/daemon/providers/claude-sidecar/test/review-guard.test.mjs apps/daemon/providers/claude-sidecar/test/review-profile.test.mjs
```

Result: **6 passed, 0 failed**. The fixtures run the real sidecar with a fake SDK
in temporary directories: read review denies writes, verify uses scratch for
writes, tested scenarios prevent commit/publish, and profile changes do not
retain earlier privileges. These are not regressions for DA-01/02 and do not
prove enforcement by the real provider's command sandbox.

Report checks: `python3 tests/hygiene/test_public_tree.py` **4 passed** (the report
was staged and therefore included in the guard); `git diff --cached --check`
passed. All 44 source references resolve to existing files with line anchors in range.

Qt fixtures were read for comparison but **not executed**: `AgentMessagingTest`
(`agentConnectionsOnlyReachAgentOperations`, token/deletion cases),
`DaemonProtocolTest` (`approvalHasOneAuthoritativeResolutionAndExpires`),
`cloud-privacy/CloudPrivacyTest` (pin enforcement, cache TTL, enrollment pin reset,
atomic sign-out rewrite, and workspace key separation). The reviewed cases cover
normal states; they do not establish rejection of the two interleavings above.

No native build/full ctest, live provider, service probing, binary installation or
deployment, private-repository access, or exploit tooling was performed.
Cryptographic, OS sandbox, and hosted-authorization behavior beyond the reviewed
code remains unverified. Areas not deeply audited include the full team/schedule
role matrix, every filesystem/attachment operation, WebRTC/native transport,
every adapter extension, and the entire legacy direct transport in `modules/ai`.

The reviewer should verify both trust-transition sequences with fake-transport
regressions before deciding severity/release disposition; the leader should open
separate remediation tasks if confirmed. Do not close the findings merely because
the six sidecar checks pass or an honest service rejects a request whose body
already contains the key envelope.
