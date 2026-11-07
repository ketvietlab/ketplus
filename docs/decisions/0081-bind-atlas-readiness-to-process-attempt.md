# ADR 0081: Bind Atlas readiness to the current process attempt

Status: accepted for readiness-banner admission; wider process and tunnel audits
remain open.

AtlasProcess passed a reserved port to the CLI but accepted the first matching
loopback URL anywhere in its output. A different or out-of-range port, an embedded
log fragment or an incomplete longer port could publish readiness. Output emitted
during `stop()` could also publish a new ready signal after cancellation began.

Only accept a complete `KetAtlas: http://127.0.0.1:<port>` line whose valid URL
names the current attempt's selected port. Support LF, CRLF, an optional trailing
slash and trailing horizontal whitespace. Continue scanning after an unsupported
banner so a later valid banner can complete the same attempt. The supported CLI
prints a complete line; a partial port cannot be accepted before its terminator.

The existing 32-KiB diagnostic tail may begin partway through a log line. Track
that boundary in the output callback so truncation cannot promote a suffix to a
banner. Clearing the buffer for a new launch resets the boundary. Suppress
readiness while stopping or without a claimed port, and return immediately after
publishing a valid endpoint. Preserve port claiming, retry and timeout behavior.

The expanded synthetic CLI regressions fail eight cases on the original source,
with six control/setup passes. The fixed source passes 14/14, including partial
and truncated lines, invalid ports, stop-time output, valid later banners and
repair/restart. Removing just the truncated-line boundary guard makes its focused
regression fail. The two affected Release/offscreen Atlas targets pass 2/2; the
existing local HTTP lifecycle check also passes with installed KetAtlas 0.6.1.

This authenticates neither the executable nor the listening process. Port
reservation races, actual bind-interface policy, total QProcess buffering,
argument/version trust, descendant cleanup and tunnel authorization remain
separate. The synthetic CLI fixture runs on Unix; Windows and native WebView
acceptance are not claimed. No UI implementation, install or deployment changes.
