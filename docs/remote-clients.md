# Serving remote clients

A paired phone or desktop reaches the daemon over an encrypted peer connection, or, while it has none, over the daemon's message socket. This page describes what the daemon does; clients follow the same limits.

## Who may connect

- Only devices pinned on this host (a device's id, key version and key fingerprint must match a pin the user approved on this host; see "Trust boundaries" in `daemon-architecture.md`) are admitted. A grant limits the connection in time and in capability; a revoked or replaced device, an expired grant or a removed pin closes its links at once.
- The daemon keeps one connection per device and numbers them by epoch; a client drops frames from an older epoch. At most eight connections are open at once.
- A peer connection must say `hello` within 10 seconds; the daemon pings every 15 seconds and closes after three unanswered pings.

## Frames and limits

- Frames are UTF-8 JSON messages of at most 64 KiB. A larger body is a transfer of up to 8 MiB on the bulk channel, in ordered parts of at most 65,524 bytes with a 12-byte header; at most four transfers are in flight, and a transfer older than 30 seconds is dropped.
- Three ordered, reliable channels carry control, events and bulk traffic. The daemon sends control before events, and keeps at most 1 MiB buffered on bulk.
- Subscriptions: a client may hold at most 16, and sends `subscribe` with a session, its generation and the cursor it last applied. The daemon pages events from that cursor, at most 100 events or 768 KiB per page, then sends live events. A client more than 2,000 events behind gets `session.reset` and loads the newest window.
- Each subscriber's live events pass a bounded queue of 4 MiB. When it fills, the daemon drops that subscriber's live events and sends `gap` with its last cursor; the client subscribes again after its cursor.
- Over the message socket a device may only subscribe and unsubscribe (at most 4 KiB per frame), and the daemon sends only `events`, `event`, `gap` and `session.reset`, at most 64 KiB each including the envelope. That read authorization lasts 10 minutes after the latest `subscribe`; before closing, the daemon sends `gap` for each subscription.

## Commands and retries

- Every command is sealed and checked as on the other command path: the grant, the capability, the device and key version, the workspace and the machine.
- A client persists a sealed command before sending it and, without a result, resends the same ciphertext with the same id. A mutating command returns the journal's recorded result, so a retry on any path never runs it twice. Each result is sealed for its delivery.
- Team commands and commands whose associated data binds service-issued state use the other command path only.

## Journal while events are forwarded

The daemon lists the event-forwarding capability in each heartbeat and follows it in each heartbeat answer.

- While the answer lists it, the daemon uploads no events and prepares no upload batches; live frames, host pages, heartbeats, session metadata, session keys, the command path and push notices go on as before. An upload already in flight finishes but sends no further chunk. The daemon remembers the state for its enrollment, so a restart does not upload the backlog before the next answer.
- When an answer no longer lists it, the daemon moves each session's upload cursor to the journal head at that moment and uploads only events journaled afterwards. Events journaled while forwarding are never uploaded. Those still within the paging window stay in the journal; the upload acknowledgement moves no further until the next acknowledged upload. A late answer to an earlier upload never moves the cursor back.
- Without uploads there is nothing to wait for, so the journal does not grow without bound while the capability stays listed. The daemon treats each session's events older than the last 2,000 as delivered, in steps of 64 events, because a client further behind than that receives `session.reset` and loads the newest window, so none needs them. A journaled event settles only its own session, from the cursor it carries and without reading the journal; each heartbeat answer settles every session once. They then go through ordinary acknowledged-event retention, which removes them in bounded passes. A client that has not acknowledged an event keeps it: retention still stops at the slowest client, however far behind. Listing the capability, withdrawing it and listing it again does not make the journal larger than that window plus what the clients have not acknowledged.

## Notifications

While events reach clients as frames instead of being stored for them, the daemon sends a small content-free notice for each event that warrants a phone notification. Notices are best effort: a small bounded number wait for the socket and the oldest is dropped first.

## Desktop event feed

The desktop's event feed and its frame codec (`modules/remote/src/control`) are the client side of the event frames. They own no socket: the caller sends what they return.

- Outgoing `subscribe` and `unsubscribe` frames stay under 4 KiB. A received frame over 64 KiB, with an unknown type, a missing, fractional or out-of-range number (cursors and generations stay within 2^53, the exact range of a JSON number), cursors that do not increase, more than 100 events or events of two generations is refused whole; the error names the reason, never event content, and no cursor changes.
- Cursors persist through `EventCursorStore`, which a local encrypted cache can implement; the desktop has no such event cache yet, so `CredentialEventCursors` keeps them in the encrypted credential store, in one record keyed by a digest of the conversation id and generation, so the store lists no conversation ids. Store use is batched: the record is read once when the feed opens, whatever the number of followed conversations, and each save is one write for every cursor that changed. A cursor is saved after each `events` page and after every 50 live `event` frames; closing the feed saves the rest. A new connection subscribes after each stored cursor. When the record cannot be read (a failed read, or a damaged record), nothing is written over it and a conversation without a readable cursor is followed in memory but not subscribed, because subscribing from 0 would replay its history; the next connection or `retryStored()` reads the store again.
- Quitting between two saves can redeliver up to 49 live events. The integrator keeps events by cursor and drops one it already has, so a redelivered event changes nothing.
- Each event is delivered once and in order. An event at or below the cursor (late or repeated) is dropped, and the cursor never moves back, except when a reload adopts a window.
- `gap` and a dropped-frames notice subscribe again after the cursor reached so far (the notice for every followed conversation, or only the one it names).
- `session.reset` starts a reload: events, gaps and drops of that conversation are ignored, and a reconnect does not subscribe it, until the caller reads the newest window and adopts its snapshot cursor; the feed then subscribes after it. A reset of an older generation is ignored, and frames of another generation than the one followed are dropped.

## Desktop reading mode

A Desktop paired as a remote client has no peer connection, so it never lists the peer-transport capability; it reads events over its message socket and lists the event-forwarding capability.

- **Capabilities.** Right after it pairs, and at each connection where the set it last had accepted differs from what it supports now, the device sends its set. An unchanged set sends nothing. A send that fails is tried again after a wait that doubles (5 s up to 5 min) while the connection stays up.
- **Reading mode.** The machine backend chooses per connection from the machine's own capabilities. Without the event-forwarding capability it polls stored events as before. With it, it never reads stored events: the device's socket and an event stream follow each open conversation from the frames of that machine. When a later connection finds the capability withdrawn, the stream closes, polling resumes, and each open conversation reloads its newest window, because the host stores events again only from the moment it was withdrawn.
- **The socket.** It opens with the device's access token, reopens with a doubling wait (1 s up to 60 s, starting over after a connection that stayed open 30 s), and is stopped for good when the device is no longer paired or its credential is revoked. Each reopen subscribes again after each conversation's cursor. A frame is wrapped for its machine and stays under 4 KiB when sent; one received over 66 KiB is ignored.
- **Following.** A conversation is followed after the cursor of the newest window the desktop read (or the last cursor it knows when a command opens it), not from its stored cursor. The stream waits before subscribing again after a drop (250 ms, doubling to 60 s while drops repeat, starting over when events arrive), so a host that is offline or a refused subscription never loops.
- **Verification.** A forwarded event is the host's sealed event: the desktop authenticates and decrypts it with the same session key and checks as a stored event. One that fails, or one that arrives without its conversation's key, is not shown; only the reason is logged, never the event. The conversation then reloads its newest window, like after `session.reset`, and the reloaded window is delivered to the view. A failed reload is tried again after a wait that doubles (up to 60 s) for as long as the conversation stays open.
- **Offline.** The host counts as unreachable while the socket is down or the host is reported offline, and as reachable again once frames of that machine arrive; each change is reported. While it is unreachable the desktop shows only what its encrypted cache and the last window hold; it fetches nothing in their place, and commands keep their own retry path. The conversation view says so in the composer's reason line, and a reloaded window replaces the transcript rows shown.
