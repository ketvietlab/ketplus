# Align với daemon / backend / mobile API

Đây là **audit code + đề xuất contract**, không phải OpenAPI mới đã được backend chấp thuận. Baseline theo [README](README.md). Backend canonical: [Control v2 OpenAPI tại 65f1e35](https://github.com/ketvietlab/ketviet/blob/65f1e35fffffd299cfb4f93f4b24982e2b1bf807/contracts/ketplus-control-v2.openapi.json); handlers ở apps/ketsuite/modules/ketplus_control/src/{auth-routes,control-routes,relay-functions}.ts. Spec hiện ghi 0.3.0-draft, Control protocol 2, servers rỗng; không suy ra URL deployment.

## Boundary giữ nguyên

~~~text
Desktop presenter → typed private domain client → authenticated local daemon IPC
                                                  ↓
                      SessionHost / SQLite journal / provider adapter
                                                  ↓ outbound only
Mobile typed client → headless KetPlus Control v2 → encrypted relay / durable jobs

Editor / files / Git / terminal / appearance → exported public KetPlus CM APIs
Atlas composition → private Atlas module → version-pinned public KetAtlas API
~~~

Desktop QML/Widgets không gọi cloud trực tiếp, không biết provider JSON-RPC. Cloud không parse/compact plaintext transcript. Mac path không là mobile identity. Worktree không đồng nghĩa với workspace tài khoản. Archive worktree cũng không phải archive tài khoản/workspace backend.

ketjs và ketatlas là dependency read-only, không phải implementation targets. Chỉ dùng public API/assets đã phát hành; không patch/fork source, thay canonical assets hoặc phát hành upstream release mới. Backend adapters/jobs thuộc ketviet; native design mapping thuộc public CM; product/Atlas composition thuộc private Desktop. Không inject code dựa vào private viewer internals để lách ranh giới này.

## Contract matrix: có thật và còn thiếu

| ID | UI use case | API hiện có tại baseline | Mismatch / công việc |
| --- | --- | --- | --- |
| A01 | Discover/attach/reconnect | IPC session.list, session.attach, session.events_after, client.ack_cursor; v0 framed JSON | Giữ session generation + cursor; tách catalog attach khỏi eager full-history replay. Version/feature negotiation trước extension |
| A02 | Repo/worktree/conversation catalog | GET /v2/mobile/sessions; ManagedSession có id/workspaceId/machineId/generation, optional repositoryId/worktreeId/labels/branch | Không có repo/worktree registry CRUD hay empty-conversation create endpoint. Identity migration, draft conversation → daemon session binding, catalog pagination/tombstones cần R02 |
| A03 | Send/steer/stop | IPC session.start_turn, session.steer, session.interrupt; POST /v2/mobile/sessions/{sessionId}/commands → **202**; GET /v2/mobile/commands/{commandId} | queued/claimed/completed/rejected/failed/expired là command lifecycle; completed của command không đồng nghĩa turn completed. UI theo event cho turn |
| A04 | Approval / user questions | IPC approval.resolve; cloud CommandKind approval.resolve; daemon ApprovalBroker | First responder/generation/expiry phải map đủ question pending/private/multi-client; payload input/approval validation không đưa raw provider request vào presenter |
| A05 | Initial 3 turn + older + live | IPC session.transcript_page; GET /v2/mobile/sessions/{sessionId}/transcript?generation=&beforeTurnSequence=&limit=3; GET .../events?generation=&afterCursor=&limit= | 🔴 TranscriptPage v1 chỉ có truncated + turn boundary; thiếu continuation trong turn/oversized item. Không được dùng snapshotCursor như proof đã nhận đủ page |
| A06 | Render canonical items/rich content | SessionEvent metadata + encrypted content; daemon event normalization/CloudEventProjection; local compact journal | Hiện payload còn chứa provider-shaped item; cần versioned normalized item identity/order/revision + media/detail policy, parity fixtures. Mobile assistantIndexes đang key theo turnSequence |
| A07 | Native sign-in/refresh/sign-out | POST /v2/auth/login, /refresh, /logout; login username/password/installationId; IPC remote.sign_in, remote.status | 🔴 CloudControlClient::handles không có sign-out IPC. Bổ sung typed sign-out + cleanup/cancel/late-response fence; logout khác machine/device revoke. Production không dùng bypass |
| A08 | Machine enrollment/presence | POST /v2/machines:enroll; GET /v2/machines; rotate-key/revoke; daemon challenges/credentials/heartbeat | Giữ trust keys/short-lived tokens; heartbeat state không chứng minh process còn sống. Không thêm form “daemon/relay server” vào user flow |
| A09 | QR claim/approve/cancel | POST /v2/pairing/tickets; mobile claim/status; Mac status/approve/revoke; IPC remote.pairing.{create,status,approve,revoke} | Binding exact claimDigest/device keys; 202 claim chưa được cấp quyền. Close phải revoke ticket; failed revoke hiện retry/expiry, không báo đã hủy |
| A10 | List/revoke devices | GET /v2/devices; POST /v2/devices/{deviceId}/revoke; IPC remote.devices, remote.device.revoke | Per-device revoke acknowledgment; late confirm không hồi sinh device; sign-out không thu hồi điện thoại đã pair |
| A11 | Encrypted replay / retries / retention | events:append; session-keys:append; mobile keys/device-keys; commands:poll + ack; maintainRelay durable job | Giữ output stripping trước encrypt và persisted retry. Cloud transcript/events đang select/filter/sort trong RAM; maintenance quét toàn bảng. Cần DB keyset/index/bounded batches; không chuyển semantic compaction lên cloud |
| A12 | Archive/delete/restore | Control v2 session archive/restore uses lifecycle revision CAS; desktop owns local worktree visibility and safe Git deletion | 🟡 Session archive/restore implemented across backend/mobile; durable cloud worktree tombstone remains separate |
| A13 | Files/Git/terminal/settings/search | Public KetPlusCM exports; private AgentWorktreeCreator/RepositoryRouter; CM SettingsPage extension | Local-only, không tạo REST không cần thiết. CM SettingsDialog là QDialog chưa có reusable embedded content/deep-link page API. Safe worktree removal phải qua owning module/interface public; private daemon giữ active-turn policy |
| A14 | Atlas index/builder/viewer/@Flow | Private AtlasIndex/AtlasTab/AtlasBuilderTab; KetAtlas CLI pinned 0.3.0 in native source | Audit public API hiện hữu; bridge/state/composition chỉ implement ở private Desktop. Không sửa/publish ketatlas. Nếu supported API không đáp ứng flow/pixel/performance, ghi blocker và xin quyết định; không dùng viewer internals. @Flow là preferred context, không security capability; không thêm cloud Atlas endpoint giả |

## Những gì source cho thấy khác tài liệu cũ

- Backend module README vẫn gọi mình là scaffold/cloud v1, nhưng control-routes.ts và canonical spec đã có v2 handlers. Cập nhật doc trong R02; code/spec/tests là căn cứ hiện tại.
- docs/chat-pagination.md còn gợi ý 20 recent turns; yêu cầu hiện tại/ADR 0004 là 3. R05 sửa doc cùng implementation.
- docs/architecture.md có đoạn sơ đồ cũ per-worktree Codex process; implementation daemon đã sở hữu provider. R04 cập nhật và không tái đưa process vào UI.
- Crypto hiện tại đã là E2EE envelope trong source; lời giải thích “chưa E2EE” từ phase cũ không còn là baseline code. Điều này **không** chứng minh secure deployment/rotation/revocation đã được kiểm thử thực tế.

## R02: các contract cần khóa trước khi giao UI

### C01 — Identity, operation state và catalog

Đề xuất domain identity bao gồm authenticated subject/account namespace, workspace, machine, stable repository/worktree, conversation, session generation, turn và item. Provider thread/item IDs chỉ là adapter references. Session ID hiện hữu được giữ; không đổi ID hàng loạt mà thiếu migration map. Local draft conversation có ID riêng trước lần send đầu; bind một lần khi daemon acknowledge, không dùng title/branch để join.

Lưu draft/attachments metadata/read anchor trong client store theo identity. Daemon/cloud giữ session/resource lifecycle, không cần đồng bộ draft qua cloud trong refactor này. Không thêm schema ownerSubjectId vào wire một cách ngầm định: auth subject và workspace ownership đang được backend resolve; extension phải ghi rõ đâu là authoritative, trường nào optional và signed/bound.

Mỗi async operation giữ immutable original scope, operation ID, expected generation/resource revision, client selection epoch. Response có thể cập nhật original store sau switch, nhưng chỉ selected matching epoch được cập nhật view/loading. Retry dùng cùng idempotency key; không xóa draft vì timeout/202. Các kết quả unknown/rejected/expired khác success. Cursor JS phải trong safe-integer range theo runtime hiện tại hoặc có versioned string encoding; không trộn int64 tùy tiện.

### C02 — Transcript page vNext, lossless và bounded

Đề xuất semantic DTO (tên wire/version cuối cùng do contract PR quyết định):

~~~text
TranscriptPageRequest:
  scope, generation, newestTurnLimit=3 OR opaqueContinuation
  requestedItemLimit, requestedEncodedByteLimit
TranscriptPageResult:
  scope, generation, transcriptVersion, snapshotBoundary
  items/events[], coveredRanges, nextContinuation?, olderTurnBoundary?
  historyAvailability, snapshotComplete, encodedBytes
~~~

1. Token continuation bind scope/generation/snapshot/query direction và hết hạn; client coi opaque. Một turn nhiều page và một item nhiều fragment/reference phải được định nghĩa, không chỉ paginate turn.
2. IPC frame tối đa hiện 1 MiB; daemon replay allowance 768 KiB và 500 events. Budget tính **toàn bộ JSON/encryption/base64 envelope**, không chỉ plaintext. Event đơn vượt budget cần fragment/large-item protocol đã xác thực; không bỏ event hoặc cho page đầu vượt frame. Đề xuất cap trả về mặc định 256 items và ≤512 KiB encoded cho vNext, đo/chốt ở R02/R14.
3. snapshotBoundary là mốc nhất quán, không phải client acknowledgement. Client có thể nhận live > snapshot trong khi history còn thiếu, nhưng giữ covered ranges / continuation độc lập; không đánh dấu missing range đã load.
4. Daemon compact projection có thể thay delta bằng item final nên cursor hiển thị không nhất thiết liên tục. Chốt coverage/compaction semantics; không áp “cursor + 1” của fake transport v0 lên mọi projection. Raw replay ack và rendered-history coverage là hai khái niệm khác.
5. Prepend/dedup theo stable item ID + revision/order; merge không lấy bản page cũ ghi đè live body mới. Duplicate conflict/stale generation reject toàn batch trước mutation. Không reset cả model khi prepend.
6. Cloud TTL tạo unavailable ranges; historyAvailability phân biệt local/retained/cache/needs-Mac/expired/unsupported/error. Mac offline không đồng nghĩa history đã mất tại Mac.
7. **Older-than-TTL qua relay chưa có read request-response mailbox trong CommandKind hiện tại.** Local dev bridge không chứng minh TestFlight đọc được history từ daemon qua cloud. R02 phải thêm authenticated observe-only history request + encrypted bounded response/timeout/cancel/TTL, không lạm dụng start_turn hay giữ nội dung lâu hơn 24 giờ.
8. Old daemon không hỗ trợ vNext: negotiated bounded legacy path có thông báo incomplete/history-unavailable. Không âm thầm quay lại parse toàn bộ journal hay giả exhausted. Advertise minimum versions/capabilities, không tự bật field mới trong response additionalProperties:false.

### C03 — Canonical item projection và render contract

Đề xuất item envelope: stable itemId + turn identity/sequence + order key + revision + kind + lifecycle + normalized payload. Tách durable event cursor khỏi display item key; assistant delta phải chỉ đích item, không chỉ turn. Fallback unknown kind giữ safe placeholder/metadata, không crash hoặc tự execute nội dung.

Kinds cần contract/fixtures: user text/attachments, assistant text (nhiều item/turn), command lifecycle, file changes, tool/progress, approval/question, compaction/lifecycle, unsupported. Rendering blocks là projection/cache versioned của client, không lưu Qt/SwiftUI/HTML markup làm canonical. Sanitization/lazy media policy không đổi nội dung text canonical.

Daemon CloudEventProjection giữ full command string + identity/status/exit code và bỏ output/outputDelta/command actions trước encryption/outbox persistence. Backend không giải mã để strip. Detail fetch command output **ngoài phase**, không thêm icon click giả dẫn đến output. Rich Markdown/code/Mermaid/file detail không bị nhầm với command stdout.

### C04 — Archive/Delete/Restore, tombstone và safe remove

Đề xuất resource lifecycle riêng connection/turn state: active, archived, delete_pending, deleted. Metadata gồm scope, resource type/id, monotonic revision, archivedAt/deletedAt, operationId, actor reference. Worktree archive là parent visibility overlay: restore parent không tự restore child đã archive độc lập; restore child khi parent archived phải giải thích/đòi restore parent trước.

- Archive: commit durable metadata rồi acknowledge; file/history và active turns giữ nguyên. Archived list có activity + Stop. Sync status pending/offline phải thật, không báo mọi client đã thấy khi mới commit local.
- Restore: authenticated expected revision; giữ original repository/worktree/conversation; missing folder/identity bị block; ở lại Settings. Không auto create folder/reclaim generation/rerun prompt.
- Delete conversation: block starting/running/waiting approval/stopping, recheck dưới session lock; tombstone + prevent new start trước purge. Daemon transaction/outbox bắt đầu xóa KetPlus journal/compact projection/key/retry references, backend purge encrypted projections; terminal outcome phản ánh phạm vi đã acknowledge. UI có delete-pending/retry khi cloud chưa ack, không báo complete toàn hệ thống sớm.
- Offline mobile không thể được xóa vật lý ngay. Bắt buộc tombstones/control metadata tồn tại **ngoài TTL content 24h** hoặc monotonic catalog epoch + forced full resync khiến stale cache bị invalidated trước render. Chốt retention/GC của tombstone trong ADR; không gom tombstone vào event TTL rồi để resurrect. Không hứa forensic/cryptographic erasure hoặc xóa provider-owned history/backups chưa nằm trong policy.
- Delete linked worktree: typed DELETE, preserve branch, archive conversations và giữ original missing-folder association. Preflight + execute phải validate registered canonical linked-worktree identity/common Git dir; block main, dirty/untracked, locked, active turns, unsaved editor buffers/active terminals cho đến khi giải quyết rõ. Không dùng recursive rm/force và không lấy path trực tiếp từ mobile.
- Public CM sở hữu generic Git/workspace remove validation/execution API, private daemon giữ session lock/fencing và private shell phối hợp unsaved buffers/terminal. Không cho public CM biết private session/credential; không mở rộng generic read-only Git service âm thầm. Nếu không bảo đảm safe remove với concurrent filesystem writes, fail closed/recheck; không hứa transaction nguyên tử giữa Git filesystem và SQLite.
- Filesystem + journal + cloud không cùng transaction: durable operation state machine có prepare/execute/reconcile, crash injection giữa từng stage. R02 phải định nghĩa recovery nếu folder đã remove nhưng metadata chưa commit. Không giả rollback được file deletion bằng một boolean UI.
- Phase Desktop khởi tạo lifecycle qua IPC; mobile phải nhận catalog/tombstones. Nếu muốn mobile khởi tạo delete, cần capability/command kind được review riêng; không mặc nhiên cấp quyền destructive cho current four command kinds.

### C05 — Query, retention và migration

Backend dùng DB-side keyset/bounded queries theo session+generation+turn/cursor, kiểm tra EXPLAIN và số rows examined trên PostgreSQL fixture. maintainRelay phải batch theo indexed expiry/due records, lease-safe/idempotent/resumable, có checkpoints/backoff. TTL là điều kiện **không được đọc sau hạn**; job chạy theo phút chỉ physical cleanup, không được gia hạn read access. Redacted rows/command/tombstone metadata cần GC policy riêng có audit safety.

Migration daemon/mobile dùng schema version, transactional upgrade, synthetic old-db fixtures, error/disk-full recovery và backup policy phù hợp quyền riêng tư. Không decode/encrypt/write toàn snapshot trên GUI/MainActor. Backfill từng batch, ưu tiên selected conversation, có progress/cancel và không chặn frame đầu. Rollback UI không rollback schema phá dữ liệu; compatibility đọc metadata/tombstones vẫn phải giữ.

## Contract delivery gate

R02 PR phải có OpenAPI diff + normalized payload/IPC schema + sanitized golden fixtures cùng hash/revision; fixtures publish ở backend contract repository và client kiểm tra pin/copy bằng hash. A schema change chỉ hoàn tất khi daemon, Desktop và iOS adapters có positive/negative tests trên cùng fixtures. Không cho TypeScript camelCase, IPC snake_case và Swift DTO tự drift.

Kiểm thử tối thiểu: wrong owner/workspace/machine/session/generation/device; expired/revoked credential/grant; duplicate/conflicting retry; 202 → final; oversized item; snapshot/page/live races; expired continuation; replay after TTL; tombstone older than content TTL; migration crash; missing folder; concurrent delete/start. Fake provider, fake clock, ephemeral DB; không quota, không real user data.

Lệnh hiện hữu: npm run test:ketplus-control ở apps/ketsuite; corepack pnpm mobile:clients:check ở backend root; npm run contract:check trong mobile. Generator mobile chung có thể không bao phủ handwritten Control v2 DTO: R02 bổ sung dedicated parity check, không lấy generated-client green làm bằng chứng thay thế. Native tests theo docs/testing.md. End-to-end staging/deployed contract verification là gate sau implementation, chưa chạy trong PR kế hoạch.
