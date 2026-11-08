# ADR 0069: Bound and verify media preview input

Status: accepted for reference-based previews; codec and broader media acceptance remain separate.

Preview checked whether an unbounded ownership-file read contained a requested
hash, then decoded the named blob without verifying its bytes. Malformed or
oversized ownership, linked reference/store roots and a valid image substituted
under an old hash were accepted. The regex also accepted a final newline on a
reference. The new temporary-image fixture reproduces seven failures, with six
existing valid/refusal controls already passing.

Resolve previews alongside the reference ownership helpers. Require exactly the
canonical sha256: prefix and 64 lowercase hex digits, regular nonsymlink media
and reference roots, and a complete successful bounded per-session ownership
read. Missing/unsupported metadata returns the existing media_unavailable result.
Reuse the same 4-MiB parser as publication and fork; no unbounded readAll remains
in this production ownership path.

Read at most the existing 16-MiB image limit plus one byte and check I/O errors.
Verify SHA-256 against the requested reference before decoding. Pass that same
bounded byte snapshot through QBuffer to the existing image codec, so it does
not reopen or continue reading a mutable file. Valid previews retain the JPEG,
size and byte-limit behavior already implemented by encodePreview; unsupported
files and metadata stay intact for repair and retry.

McpMediaReferences.cpp owns reference lookup and validation; McpMediaPreview.cpp
continues to own decode/scale/JPEG encoding. No shared header, UI implementation,
retention default or public result shape changes. The focused fixture passes
15/15 including setup/cleanup, covers both old refusals and new guards, preserves
unsupported bytes and verifies successful JPEG output after repair. Its POSIX
mode-denial row explicitly skips on Windows. Other Windows verification is not
claimed.

This does not establish descriptor-relative ancestor confinement, cross-file
snapshot isolation or a power-loss guarantee. Pixel limits remain the existing
codec policy, not new memory/concurrency acceptance evidence. Existing orphan,
write-admission and full media/archive requirements remain in H1.4/H1.5. No live
user media, paid provider, installation or deployment was exercised.
