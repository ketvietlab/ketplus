# Paired team member assignment roles

The public daemon uses the same `team.member.add` operation for local clients and
paired clients. The additive `role` field aligns paired requests with the existing
local Developer/Reviewer behavior. It does not grant the caller an agent role.
The role-less paired payload remains supported.

## Request contract

The paired whitelist is exactly `team_id`, `role`, `name`, `lane`, `paths`,
`provider`, `model`, `effort`, `expected_revision`. Other fields remain rejected.
In particular, `actor`, `session_id`, `execution_policy` and `risk_acknowledged`
are not additional member-add permissions.

| Input | Meaning |
| --- | --- |
| `role` absent | Create a `developer`, preserving the legacy default |
| `role: "developer"` | Create a developer |
| `role: "reviewer"` | Create a reviewer, subject to the existing reviewer policy and limit |
| Any other string, including `leader`, empty, whitespace or case variants | `invalid_payload` |
| Explicit null, boolean, number, array or object | `invalid_payload`; only absence selects the default |

An invalid role returns `{ok: false, error_code: "invalid_payload", error_message:
"A member added to a team is a developer or a reviewer."}` without creating a
conversation or adding a member. The local path applies the same typed rule;
malformed values that previously fell through JSON string conversion are rejected.

A developer requires a valid `name`; its omitted/empty lane falls back to its name,
and its paths use the existing normalization and overlap rules. A reviewer with no
name uses `Reviewer 2`; it does not receive developer lane/path assignments. Provider,
model and effort parsing is unchanged. The returned team/read projection already
carries each member's role, so no response version or field is added.

## Fences and existing failures

A paired change still requires `expected_revision` and `teams.write` in the authenticated
paired transport. It is fenced against the member-list revision; stale changes return
`stale_team` with the current team. Re-delivery with the same message identity keeps
existing command-result idempotency. Invalid payloads do not become valid through
that cache. Existing actor, owner, device-key, session scope and pairing checks remain
in place; a requested reviewer is not a leader or an actor credential.

The local mutation path retains these failures:

- `team_full`: at most five developers.
- `role_taken`: at most two reviewers; no new leader can be requested here.
- `provider_unavailable`: unknown or unavailable provider.
- `reviewer_unsupported`: provider/execution policy cannot review. A new reviewer
  cannot request or acknowledge a broader execution policy in this payload; existing
  team policy and provider-specific reviewer policy decide support.
- `member_unavailable`: creating the member conversation failed.
- `lane_overlap` and existing path/name validation: unchanged developer rules.

The strict paired whitelist changes only by adding `role`. Review routing, task
acceptance, actor authorization and execution-policy acknowledgements are unchanged.
Older daemons reject an explicit role they do not support. A client must not silently
retry a rejected reviewer request without role, because that would create a developer.
For developer creation, omitting role remains the backward-compatible payload.

## Verification and ownership

The public implementation is in `SessionHostTeamsRemote.cpp` and
`SessionHostTeamsProtocol.cpp`. Client-specific schema, payload and presentation work
belongs to the client owner and is not implemented or copied here.

Run the actual host fixtures `ketplus_team_remote_tests`,
`ketplus_team_remote_cloud_tests` and `ketplus_team_protocol_tests`, together with
`ketplus_team_store_tests`, `ketplus_team_reviewers_tests` and
`ketplus_team_review_policy_tests` as the directly affected rules/provider-policy
fixtures required by
[testing.md](testing.md#minimum-verification). The cases cover explicit/omitted roles,
malformed/leader/actor/policy injection, no partial member/session creation, replay,
member limits, reviewer support, stale revision and authenticated paired scope.
These use temporary repositories and fake providers, without provider quota.
Report the exact source revision, execution platform and any unavailable matrix;
source inspection and formatting do not substitute for executing the fixtures.
