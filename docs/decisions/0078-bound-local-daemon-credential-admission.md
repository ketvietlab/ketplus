# ADR 0078: Bound local daemon credential admission

Status: accepted for credential-file reads; endpoint and directory trust remain
under audit.

The daemon and its Qt client read the entire local credential file before checking
its minimum length. The client also accepted symlinks and group-readable files.
An oversized file could allocate unbounded input or be trimmed into an apparently
valid token. The two readers disagreed about which existing credentials to trust.

Use one Qt Core helper in `ketplus_remote` for daemon startup and client hello.
Limit raw input to 4096 bytes before whitespace trimming; require at least 32
trimmed bytes. Generated 43-byte base64url tokens and existing 32-byte or longer
credentials remain compatible within the bound. This is a file-size admission
limit, not a change to token generation or a new encoding requirement.

On Unix, open with `O_NOFOLLOW`, `O_NONBLOCK` and `O_CLOEXEC`, then inspect the
descriptor with `fstat`: regular file, current UID, no group/other permission
bits. Check its size and read at most 4097 bytes to detect growth after `fstat`.
Do not rely on a separate pathname permission check. Close the descriptor on
every return. Other platforms retain regular-file/symlink checks and the bounded
read, without claiming Unix ownership guarantees or Windows ACL validation.

Refusal preserves the existing file and returns a static error without credential
contents. The server does not create over a dangling link. A missing credential
still uses the existing owner-only atomic creation path. A client refuses hello
and uses its existing reconnect policy; repairing the file allows a later attempt.

Synthetic service/client tests reproduce six original failures, with twenty
passing control/setup cases. All 28 focused cases pass after the fix, including two additional FIFO cases. They cover
4096/4097-byte boundaries (including whitespace that trims below the limit),
generated and legacy credentials, short/empty files, directories, Unix symlinks
including dangling links, group-readable files, FIFOs, preservation and repair/retry.
No actual credential, provider account or installed daemon is used.

The claim is limited to reads of the opened token file. Ancestor-directory races,
creation/replacement races, mutation by the owning user, endpoint identity,
non-Unix ACLs and cross-platform acceptance remain separate. The tests do not
impersonate a different UID or exercise a concurrent substitution race. Shared build
and API changes require the full Pro suite and independent exact-head review.
