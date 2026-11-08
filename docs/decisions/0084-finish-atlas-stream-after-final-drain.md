# ADR 0084: Emit Atlas stream EOF after its final drain

Status: implemented and locally verified; independent review pending.

An Atlas peer may replenish credit in 64-KiB windows rather than after each
frame. After the local TCP origin closes, the final buffered response can drain
from the WebSocket bytes-written callback. The previous code emitted EOF only
from TCP disconnect or receipt of another credit frame. A peer that already
received the full body but did not send more credit could wait forever for EOF.

After each successful drain, recheck the stream and emit its end frame/remove
it when the TCP socket is disconnected and its read buffer is empty. A failed
data send stops processing before the recheck. Existing disconnect/credit paths
already recheck stream existence, so they do not send a second end for a stream
retired by drain. Frame, stream, socket and credit limits remain unchanged.

The encrypted loopback regression covers per-frame, half-window and full-window
credit replenishment, plus a response smaller than the initial window. The full
window case delays replenishment so TCP EOF arrives while buffered bytes await
credit; it verifies every response byte before requiring exactly one end. The
original source receives the complete response but fails the EOF assertion.
The fixed focused suite passes 7/7; the four directly affected runtime, tunnel,
cloud-privacy and remote-client targets pass. Real CLI remains opt-in/skipped.

This is stream completion, not acceptance of all tunnel/grant/lease, origin/path
confinement, peer authentication, fairness or process-descendant behavior. Tests
use synthetic loopback TCP/WebSocket services and fixed test keys, without a
provider, installed renderer, real relay, UI, deployment or device installation.
