# Transcript V2 — kế hoạch triển khai

## Chiến lược

Thay presentation pipeline qua seam mới, không sửa dần giant renderer hiện tại. Daemon journal và
protocol là nền ổn định; migration chủ yếu nằm ở Desktop projection/model/view.

Không xóa code cũ trước cutover. Hai pipeline có thể cùng tồn tại ngắn hạn sau feature flag, nhưng
chỉ một pipeline được quyền hiển thị trong một session để tránh hai nguồn sở hữu scroll/state.

## PR 0 — inventory và characterization

- Tạo event corpus từ schema, fixture và session thật.
- Điền coverage ledger cho mọi `type/subtype` quan sát được.
- Characterize live/replay/pagination, command-output privacy, images và unknown event.
- Chốt cách phân biệt commentary với final; nếu protocol thiếu phase, ghi rõ protocol addition.
- Chụp baseline CPU/memory/scroll và screenshot light/dark.

**Gate:** không còn event quan sát được nhưng chưa có expected canonical projection.

## PR 1 — canonical event và normalizer

- Tạo `TranscriptEvent` typed cùng stable identity.
- Tạo `ActivityNormalizer` thuần/deterministic cho raw item lifecycle.
- Merge start/delta/complete theo item key; retry là attempt riêng.
- Thêm sanitizer chung cho command, URL và tool payload.
- Thêm generic `unknown` event; cấm drop raw item không nhận diện.
- Viết equivalence test: normalize live stream và normalize replay cho kết quả giống nhau.

**Gate:** corpus unit test qua; unknown fixture sinh row; không có QML trong PR này.

## PR 2 — model/controller đơn nguồn sự thật

- Một `TranscriptModel` sở hữu canonical items của conversation đang chọn.
- Một `TranscriptController` nối window, live delta, pagination và session generation fencing.
- Activity grouping là proxy/presentation model hoặc hàm dẫn xuất; canonical model không chứa group.
- Streaming coalescing nằm một chỗ; completion flush ngay.
- Large payload/thumbnail/output dùng reference và lazy load, không copy nhiều lần qua `QVariantMap`.
- Conversation switch hủy generation cũ; late delta không được lọt sang conversation mới.

**Gate:** switch conversation, load older và restart không duplicate/reorder/mất item.

## PR 3 — transcript shell và renderer registry

Tạo các component nhỏ:

- `AgentWorkspace.qml` transcript viewport (một virtualized `ListView` duy nhất)
- `TranscriptTurn.qml`
- `ActivityGroup.qml`
- `GenericActivityRow.qml`
- `FileActivityRow.qml`
- `CommandActivityRow.qml`
- `SearchActivityRow.qml`
- `BrowserActivityRow.qml`
- `ImageActivityRow.qml`
- `IntegrationActivityRow.qml`
- `ErrorActivityRow.qml`

Tái sử dụng question/image primitives chỉ khi chúng không mang state cũ. Nếu primitive cũ kéo theo
warm view, character anchor hoặc giant delegate thì viết component nhỏ mới.

Scroll state machine chỉ gồm:

```text
FollowingLatest ──user scrolls up──▶ ReadingHistory
ReadingHistory ──jump/scroll bottom──▶ FollowingLatest
```

Mutation ở `ReadingHistory` giữ `{topRowKey, rowPixelOffset}`; không dùng correction loop nhiều frame.

**Gate:** user/assistant/activity/final render đúng; unknown row visible; streaming không kéo viewport.

## PR 4 — feature parity mở rộng

Thực hiện theo thứ tự để mỗi lát cắt độc lập kiểm được:

1. Command lifecycle, exact command, cwd, exit và output detail.
2. File read/create/edit/delete/move cùng file-group detail.
3. Web/code search.
4. Browser open/navigate/click/download/screenshot và URL redaction.
5. Image view gallery cho 1/N ảnh, lazy thumbnail và preview.
6. MCP/dynamic/app/connector activity.
7. Approval, structured question, error và retry.
8. Artifact/link/Mermaid và copy/export.

**Gate:** mọi dòng trong coverage ledger có renderer hoặc quyết định dùng generic fallback được ghi rõ.

## PR 5 — persistence, remote và performance

- Replay từ daemon tạo đúng canonical item/group như live.
- Pagination thêm history mà không thay identity item hiện có.
- Remote projection giữ privacy boundary; không vô tình gửi command output/raw secrets.
- Bounded cache cho thumbnail, detail và rendered Markdown.
- Streaming final chỉ parse Markdown theo policy đã chốt, không parse toàn response trên mỗi delta.
- Benchmark 1k/10k activity rows, 50 delta/s, 1/4/50 images và giant response/output.

**Gate:** equivalence, privacy, bounded-memory và benchmark regression qua.

## PR 6 — cutover và xóa pipeline cũ

Sau khi tất cả gate trước xanh:

- Chuyển production seam sang V2.
- Chạy full private suite và manual QA bản build thật.
- Xóa warm transcript view cache, legacy position helpers, duplicate render cache và transcript block
  cũ trong `AgentWorkspace.qml`.
- Xóa test chỉ bảo vệ implementation hack; giữ/viết lại behavioral tests.
- Cập nhật architecture, agent workspace, pagination và project status docs.

**Gate:** không còn hai source of truth hoặc hai transcript view; release build chứa đúng daemon và
Desktop, smoke test từ DMG đã mount.

## Quy tắc review theo PR

Mọi PR transcript phải trả lời trong description:

1. Raw event nào được thêm/thay đổi?
2. Coverage ledger và fixture nào chứng minh?
3. Live và replay có cùng kết quả không?
4. Unknown event xử lý thế nào?
5. Có tăng payload/copy/cache không; giới hạn là gì?
6. Dữ liệu nào được redact?
7. Scroll/focus/accessibility bị ảnh hưởng ra sao?

Thiếu một câu trả lời thì PR chưa sẵn sàng review.

## Rollback

- Feature flag chỉ dùng trong giai đoạn cutover, không giữ vĩnh viễn.
- Rollback quay về legacy presentation nhưng không downgrade journal/protocol.
- Schema canonical mới phải đọc được event cũ; không migration phá hủy transcript đã lưu.
- Nếu replay mismatch hoặc privacy test fail, dừng release thay vì fallback im lặng sang row mất dữ liệu.
