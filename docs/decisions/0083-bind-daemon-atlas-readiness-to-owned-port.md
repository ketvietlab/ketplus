# ADR 0083: Bind daemon Atlas readiness to its owned process attempt

Status: implemented and locally verified; independent review pending.

The daemon's `AtlasLiveRuntime` used substring readiness matching independently
of the desktop Atlas process. A longer numeric port, embedded log text or an
unterminated line could publish an attachment for a process that had not reported
the owned viewer endpoint. Trimming its diagnostic buffer also lost knowledge of
whether the first retained byte was a line boundary.

Accept only a complete line beginning `KetAtlas: http://127.0.0.1:PORT`, with
the numeric port equal to the current viewer port. LF, CRLF, an optional slash
and trailing horizontal whitespace are supported. Ignore partial, embedded,
wrong-port and post-stop banners. Preserve line-boundary state when trimming the
32-KiB diagnostic tail and reset it when launching the owned viewer. A later
correct banner can still establish readiness.

The start result must be delivered once even when the process exits immediately
after its banner. Move alone does not guarantee an empty source `std::function`;
exchange the stored callback with an empty function before invoking it. Finish
internal timer/connection setup before the success callback, and stop/schedule
cleanup before a failure callback. Ignore repeated failure after stop.

Synthetic Python CLI fixtures exercise complete/split banners, incorrect ports,
log-prefix/truncation boundaries, stop-time output, repaired fresh-process retry
and immediate process exit. The original source fails five cases; the corrected
focused suite passes 15/15 including setup/cleanup. Removing the boundary guard,
stop guard or one-shot callback exchange each makes its focused regression fail. The complete Release Pro build and 96/96 target suite
pass; the real CLI opt-in remains skipped.

This preserves the daemon's existing 0.5.2 CLI compatibility requirement and
read-only viewer arguments. It does not authenticate the executable or listener,
remove the reserve/release port race, bound the initial `readAll` allocation,
prove renderer/descendant cleanup or accept all tunnel/grant/lease behavior.
The real CLI opt-in remains separate; no installed CLI, user workspace, provider,
UI, relay deployment or device installation is exercised by these fixtures.
