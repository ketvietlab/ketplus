# ADR 0079: Bound the local client's unread IPC buffer

Status: accepted for the Qt client transport buffer; wider IPC resource and
lifetime bounds remain under audit.

DaemonClient checks individual frame lengths, but its QLocalSocket used Qt's
unlimited default receive buffer. Pausing application dispatch could accumulate
an arbitrary response stream before the frame parser got a chance to reject it.
The server-side change in ADR 0077 does not configure this separate client socket.

Set the client's socket read limit to the 1-MiB maximum frame plus its four-byte
header in the constructor, before connection or hello. Use transport backpressure
rather than discarding valid pipelined frames. Keep framing, authentication,
reconnect policy, queued calls and the wire limit unchanged. No UI implementation
is changed.

Two synthetic local-server regressions pause client application dispatch before
and after hello. Three exact-limit frames occupy 3,145,740 unread bytes before the
fix and at most 1,048,580 afterwards. Once dispatch resumes, every frame and one
later event are received on the same connection. The focused client policy target
passes 9/9 including setup/cleanup and existing upgrade/compatibility controls.
A friend accessor observes the actual private socket in tests without adding a
public runtime API. This shared-header change requires full Pro validation.

This only bounds Qt's unread transport buffer. Parsed JSON, event subscribers,
the frame assembler, OS buffers, write queues and pending requests have separate
costs. Per-turn request fairness, total memory, callback lifetime, timeouts and
cross-platform acceptance remain open. The loopback probe does not use a real
daemon, provider, account or user credential.
