# ADR 0036: Domain-separated daemon challenge signatures

Status: accepted contract; relay/client rollout tracked in #85.

## Decision

The daemon identity key also signs event envelopes, wrapped session keys and
WebRTC answers. Authentication must never expose a signing oracle for those
protocols. The previous 32-byte-only nonce restriction remains a legacy
mitigation; the new client uses only the following domain-separated scheme.

Both challenge and credential POST bodies carry numeric `signatureVersion: 2`
and the case-sensitive string `signatureAlgorithm: "Ed25519"`. Challenge issuance
still sends `machineId` and `keyVersion`; redemption still sends `challengeId`
and uses the `X-KetPlus-Machine-Signature` header for unpadded base64url signatures.
The challenge reply retains `id`, `challenge`, `expiresAt`, and adds both scheme
fields plus `machineId` and `keyVersion`.

Signing bytes, with no trailing newline, are:

```text
UTF8(compact JSON ["ketplus",2,"daemon-challenge",id,machineId,keyVersion])
|| 0x00 || decoded_nonce
```

- IDs match `[A-Za-z0-9_-]{1,128}`; no escaping is needed for these ASCII values.
- `keyVersion` is an integer from 1 through 2147483647, serialized as a JSON number.
- The reply's machine and key version must match the enrollment requesting it.
- `challenge` is canonical unpadded base64url for exactly 32 bytes (43 characters).
- The scheme fields must have their declared types and exact supported values.
- No whitespace, BOM or newline is present in the JSON bytes.

The public test vector is in
`tests/daemon/fixtures/daemon-challenge-v2.json`. Its public key is the published
RFC 8032 test key, not a deployed identity. C++ checks the exact bytes and verifies
the supplied signature independently with OpenSSL; HTTP tests also verify the
signature emitted by the actual client.

The relay binds the persisted challenge to machine, key, scheme and expiry and
atomically consumes it with credential creation. Scheme mismatch is rejected;
failed v2 verification must never fall back to v1. Relay implementation remains
in its own repository.

## Compatibility and rollout

| Client | Relay | Behavior |
| --- | --- | --- |
| Legacy | Legacy | Existing raw-32-byte authentication |
| Legacy | V2 with legacy enabled | Legacy only when both scheme fields are absent |
| Legacy | V2 with legacy disabled | Authentication rejected |
| V2 | Legacy | Fail closed on absent/mismatched reply fields or unsupported request |
| V2 | V2 | Domain-separated authentication |

Deploy and verify the relay first, release the client next, then disable legacy
after old clients are retired. The agreed relay compatibility flag is
`KETPLUS_ALLOW_LEGACY_DAEMON_CHALLENGE`; `false` blocks legacy issuance and redemption,
including outstanding old challenges. Legacy is initially enabled for relay-first
rollout. Explicit version 1, partially supplied scheme fields, null or unknown
request schemes are rejected. Only stored challenges whose two scheme fields are
both null from before migration can use the legacy branch under that flag.

The new client never retries legacy. Cached credentials remain valid until their
normal refresh boundary. Rolling the relay back to legacy while these clients
are deployed breaks refresh; restore v2 support rather than weaken client
validation. This PR does not deploy either component or establish that legacy has
been disabled. #85 stays open until coordinated rollout is verified.
