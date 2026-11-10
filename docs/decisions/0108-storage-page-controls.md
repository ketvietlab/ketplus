# ADR 0108: The Storage page's controls and jobs

Status: accepted (2026-11-10, #128). Changes ADR 0046 (the size target applies by default)
and ADR 0100 (the archive period is a setting, 90 days by default).

## Context

#128 asks for a Storage page that shows what the journal holds and lets the user bound it,
plus warnings in the sidebar and on remote clients. The backend already trimmed old detail
(ADRs 0037–0044, 0098–0101), returned free pages (incremental vacuum) and kept a revisioned
size target (ADR 0046). These were still missing:

- the Generated images and Archived conversations choices;
- a warning the user can turn off;
- Compact now with progress, the last compaction and the oldest full detail;
- the size by part and the largest conversations;
- Export;
- a low-disk warning.

The size target was also opt-in: it did nothing until the user saved it once.

## Decision

- **Storage options.** They are a third revisioned policy, `journal_storage_options`,
  read and written through `storage.options.read` and `storage.options.apply`. They are
  kept apart from the detail and size policies, so an older client that saves one of those
  never resets them. The options:
  - `archived_history_days`: 30, 90 (default), 365, or 0 for Never;
  - `generated_images`: `full`, `preview` (default) or `caption`;
  - `warn_near_limit`: true by default.

  Strict three-field JSON, expected-revision CAS and fail-closed reads match ADR 0042.
  If the stored options cannot be read:
  - archive expiry pauses;
  - generated images are kept whole.
- **Archive expiry** reads the archive period from the options.
- **Generated images.** Older detail follows the option:
  - Full skips `imageGeneration` items without reading their pixels;
  - Caption compacts them without a preview;
  - Preview keeps the ADR 0101 behaviour.

  Images inside tool results keep their own policy (ADR 0099).
- **The size target applies by default.** `pressureConfigured()` no longer requires a
  saved revision. A new journal therefore trims eligible detail earlier while it is over
  5 GB.
  - The guards are unchanged: running turns, the latest 20 turns, questions and approvals,
    and unacknowledged events.
  - Forever and automatic-off still disable detail trimming.
  - Desktop applies a new target as soon as it is chosen, like every other setting
    (ADR 0103). The warning threshold keeps its stored value.
- **Status.** `storage.size.read`, `storage.options.read` and `session.list`
  (`journal_storage`, for remote clients that cannot call `storage.*`) report:
  - `database_page_bytes`, `limit_bytes` and `over_limit`;
  - `warning`: over the limit, or past the threshold while `warn_near_limit` is on;
  - `low_disk`: under 5 GB free on the journal's file system;
  - `filesystem_available_bytes`.
- **Compact now.**
  - `storage.compaction.start` starts a run, and `storage.compaction.read` reports it.
    `storage.size.read` also includes the run as `compaction`.
  - The run repeats the idle pass with a 10 ms pause between passes: acknowledged events,
    attachments, retention, 128 MB of free pages and archive expiry, then batches of 256
    replay rows for up to 50 ms. A real journal has about a million replay rows, and one
    batch per pass made a sweep take several minutes.
  - It waits, in `waiting_for_idle`, while a session is active.
  - It ends `done` once a whole detail sweep after the last change found nothing to trim or
    return. A run also ends `stopped` after 30 minutes, or `failed` after 50 writes in a
    row that failed.
  - A finished run is stored in `journal_compaction_state` and reported as `last`.
  - Progress is reported as the current sweep's fraction and the bytes before and now.
- **Breakdown.**
  - `storage.breakdown.read` measures on a worker thread through its own read-only
    connection, so the daemon keeps serving clients meanwhile. It returns `measuring`
    until the result is ready, and keeps a result for 10 minutes or until the next
    compaction.
  - The parts:
    - generated images: completed image items plus their large payloads;
    - activity and usage index: dbstat pages of the `activity_*` tables plus usage rows;
    - free pages;
    - conversation history: the rest. The four parts add up to the database pages.
  - It also returns the five largest conversations, with title, provider, number, bytes
    and the kind of data that dominates them, plus the oldest replay row and the journal's
    folder.
- **Export.**
  - `storage.export.start {directory}` writes into a new owner-only
    `KetPlus conversations <time>` folder inside an existing absolute directory. It
    writes one owner-only JSON file per conversation (`ketplus.conversation-export/1`):
    the conversation's metadata and its replay events, with large items put back together.
  - It runs on a worker thread, and `storage.export.read` reports `exported`/`total`.
- **Access.** Every `storage.*` command stays local-owner only. Cloud control is refused,
  as before. Starting a job and saving the options are mutating, deduplicated commands.
  Nothing here starts a provider.
- **Desktop.**
  - The Storage page follows the mock. Its overview has:
    - the SQLite files and the free disk;
    - the over- or near-limit banner with Compact now, and the low-disk banner;
    - the Journal meter, with the folder and "full detail since";
    - the four part meters.

    Below the overview come the retention rows, the two checkboxes, the largest
    conversations, and the last compaction with Compact now and Export.
  - The sidebar banner reads "Journal is 9.0 GB · Over its 5 GB limit" (or "Near its…",
    or "Disk space is low"). Clicking it opens Settings → Storage. It follows each Storage
    read, so it clears after a compaction.
  - Compacted commands and generated images say what went, for example
    "Output trimmed · 1.2 MB · older than 30 days".

## Consequences

- Journals that never saved a size target start trimming eligible recent detail while
  they are over 5 GB. This is what #128 asked for. The guards keep recent work whole.
- The mock's notes about an explicit target save and its design-preview line no longer
  apply, so the page omits them.
- The size check in `docs/testing.md` compacts a copy of a real journal through the same
  commands.
