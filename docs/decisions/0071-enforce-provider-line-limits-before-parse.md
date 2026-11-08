# ADR 0071: Enforce provider line limits before parsing

Status: accepted for Claude/Cursor stdout; broader provider framing remains under audit.

ClaudeAdapter and CursorAdapter rejected a stdout buffer over 16 MiB only when
that buffer had no newline. A complete oversized line, or a valid line followed
by an oversized unterminated line, bypassed the limit. Deterministic tests load
real QProcess output before its read notification: the original adapters fail
eight cases while six valid/refusal controls already pass.

Read stdout in 64-KiB chunks. For every buffered raw line, check its length before
trimming or parsing; also check the unfinished suffix. Accept the existing exact
16-MiB boundary and independent valid lines even when their aggregate exceeds it.
The adapter's accumulated raw-line buffer is at most the limit plus one read chunk;
this is not a bound on QProcess internals, parser allocations or event-loop time.

Preserve the existing oversized-message failure policy: fail pending calls,
report the error and kill the process. Close the stdout read channel and drain
already-received bytes in bounded chunks before invoking callbacks, so a later
notification or finished callback cannot publish the rejected suffix. Qt documents
that [closing the read channel](https://doc.qt.io/qt-6/qprocess.html#closeReadChannel)
keeps already-received bytes readable, hence the explicit drain. A fresh process
generation reopens its channel and has no discarded prefix.

The fixture passes 16/16 with exact-boundary, aggregate, complete/partial oversized,
valid/blank-prefix cases for both adapters. It checks request signals before and
after the failure, repeat notification, complete drain and a successful new
process generation. Existing real-adapter/fake-provider suites provide the broader
lifecycle gate. No provider quota, live credentials, UI implementation, user data,
installation or deployment is involved; Windows execution is not claimed.

Keep this localized in each adapter's existing stdout owner. CursorAdapter is an
existing file above 1,000 lines; this change replaces its reader rather than moving
unrelated ACP state and callbacks during a framing fix. Broader splitting remains
separate. Codex oversized-message continuation, stderr, version-probe output,
UTF-8 acceptance and event-loop fairness remain separate H3.1 work.
