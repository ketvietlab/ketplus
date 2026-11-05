# Bounded automatic history loading

## Cause and change

One-request-in-flight protected concurrent requests but not sequential requests.
Wheel events and contentY changes repeatedly requested older pages while the
reader stayed near the top. Fast completion and continuing momentum could drain
many pages without another intentional gesture.

The per-ListView gate now permits one automatic page per gesture. Wheel events
within 400 ms share a gesture. Only new input after that quiet interval, a new
scrollbar press, or a new direct drag can re-arm it. Timer expiry, page completion
and layout/anchor adjustments never re-arm it. Input during loading is consumed,
not queued. Upward drag direction is checked and the explicit button remains.
No timer work is started by wheel input when there is no older history.

The gate stays with the existing ListView because it owns its wheel, drag and
scrollbar state; this narrow fix does not extract the existing large QML file.
Daemon/storage, newest-three-turn limits, collapse and persisted history are
unchanged. This is a UI request gate, not a hard resident-history memory cap.

## Verification

- Release build passed.
- Final full private CTest run: 19/19 targets passed, 38.67 seconds.
- New viewport regression covers initial/programmatic geometry, fast completion,
  prepend plus 40 continuing input events, quiet expiry without input, fresh
  input, input during loading, explicit button and repeated scrollbar holds.
- Native GUI and real touchpad validation are still pending. The quiet interval
  is a gesture heuristic, not an OS gesture identifier.
- An intermediate full run observed a failure in the existing
  scrollingWhileStreamingKeepsReadingPosition:mixed-mouse case (step 47, row 115,
  displacement -271.781 px). Isolated reruns of that case on the intermediate
  binary and the earlier performance binary passed. The final full run passed.
  Root cause of that observation is not established; no assertion or timeout was
  weakened, and this fix does not claim all streaming anchor problems resolved.

No installed application or running daemon is replaced by this change.
