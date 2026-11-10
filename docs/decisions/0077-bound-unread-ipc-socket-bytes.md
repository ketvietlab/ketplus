# ADR 0077: Bound unread IPC socket bytes

Status: accepted for the server-side Qt read buffer; broader IPC lifetime and
request-work limits remain under audit.

LocalIpcServer bounds individual frames and queued writes, and stops dispatching
requests when replies create backpressure. QLocalSocket's internal receive buffer
was left unlimited, so pausing application reads did not bound incoming memory.
Qt documents this default and the receive cap in
[QLocalSocket::setReadBufferSize](https://doc.qt.io/qt-6/qlocalsocket.html#setReadBufferSize).

Set the accepted socket's read limit to one maximum frame plus its four-byte
header, before either connection admission or authentication. The existing frame
assembler, outbound cap, hello deadline, authentication and connection-count
policies are unchanged. This is socket backpressure, not dropping excess bytes or
rejecting a client simply for pipelining valid frames.

Two loopback regressions hold application dispatch using QSignalBlocker while the
transport event loop keeps receiving. Three valid exact-limit frames grow the
original unread buffer to 3,145,740 bytes, before and after hello. With the cap it
stays at 1,048,580 bytes. Releasing dispatch consumes all three successful replies
and a subsequent request on the same connection. Focused checks pass 4/4 including
setup/cleanup; no live provider or external service is used.

The claim is only the Qt unread transport-buffer bound after acceptance. It is
not a process-wide RSS cap: the frame assembler, parsed JSON, responses, OS socket
buffers, connection churn and other clients also use memory. General request
fairness, pre-admission/closing-peer counts, platform-specific buffering and
Windows acceptance remain separate. Run the directly affected daemon/desktop IPC
matrix and independent exact-head review; no UI implementation is changed.
