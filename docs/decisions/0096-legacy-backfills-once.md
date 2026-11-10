# ADR 0096: Legacy journal backfills run once

Status: accepted; opening a stamped journal does not rescan its history.

Two startup backfills exist for journals from before their columns:
`last_turn_started_at` from the latest user message, and turn numbers plus
titles from raw events. Both selected every session whose value was still empty
and scanned that session's raw (and replay) rows on every daemon start. A
session whose first prompt had no text, or that never had a user message, kept
the empty value forever, so the scan repeated each time. A temporary Release
fixture with 300k raw and 300k replay rows over 20k sessions measured about
360 ms of such work per start once all one-time migrations had finished (#68).

`LocalJournal` keeps the `PRAGMA user_version` it found at open (ADR 0095). Both
backfills run only for a journal below format 1. A journal reaches format 1 only
after a successful open that ran them, and daemons that stamp format 1 write
these values for every new turn, so a stamped journal needs neither again. The
same fixture now opens in 16 ms after the first open. Tests that simulate a
pre-versioning journal reset `user_version` to 0, as such a journal reads.

The one-time activity-index backfill for sessions without activity heads still
runs synchronously on the first open after upgrade (5.3 s in that fixture); it
remains a separate #68 startup gate. No schema, protocol or UI change is
included.
