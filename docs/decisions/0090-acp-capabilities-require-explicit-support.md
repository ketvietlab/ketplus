# ADR 0090: Require explicit ACP capability support

Status: accepted for ACP v1 loadSession and image negotiation.

The runtime narrowing code removed these capabilities only for explicit boolean
false. Omitted fields, nulls and wrong types retained the baseline flags. Cursor
could therefore call session/load even when initialization did not advertise it,
and report image support without an affirmative prompt capability. An old test
incorrectly encoded omission as permission.

ACP v1 gives both capabilities a false default. See the primary protocol sources:
[checking session-load support](https://agentclientprotocol.com/protocol/v1/session-setup#checking-support)
and [prompt capabilities](https://agentclientprotocol.com/protocol/v1/initialization#prompt-capabilities).
Only a JSON boolean true now preserves the corresponding baseline flag. Missing,
null and wrong-type values remove it. Negotiation can narrow baseline support;
it cannot add a feature the adapter does not implement. Existing per-process reset
permits a fresh generation to advertise capabilities that an earlier one lacked.

Direct regressions cover omitted/nested/wrong-type fields, both boolean controls
and reset. Cursor's process fixture also checks that omitted/mistyped loadSession
never causes an actual session/load call. The normal fake Cursor now explicitly
advertises the load/image features it implements. Against original production,
compatibility has 20 passing / 12 failing checks and Cursor has 24 passing / 3
failing checks. The fix must pass these tests and the final integrated Pro suite.

This completes the concrete capability finding in the #71 contract package. It
does not validate every live CLI version or introduce protocol v2 support. Request
admission/timeouts and malformed method schemas remain separate. No UI,
provider quota, live journal, installation or deployment changes are included.
