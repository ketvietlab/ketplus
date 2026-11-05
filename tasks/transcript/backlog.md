# Transcript V2 — implementation backlog

Trạng thái dùng trong bảng: `Todo`, `Doing`, `Blocked`, `Done`. Chỉ chuyển `Done` khi acceptance của
item đã có bằng chứng test hoặc screenshot tương ứng.

## Wave 0 — inventory và contract

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-001 | Done | Chụp schema/event inventory ở commit mục tiêu | Mọi observed type/subtype nằm trong ledger |
| TRN-002 | Done | Thu corpus từ session thật | Có inventory aggregate từ journal thật và fixture payload đã loại dữ liệu người dùng |
| TRN-003 | Done | Chốt phase commentary/final | Không phân loại bằng heuristic text |
| TRN-004 | Done | Characterize live/replay/privacy | Expected fixtures được review |
| TRN-005 | Todo | Chụp performance/visual baseline | Kết quả, máy và fixture được ghi |

## Wave 1 — data pipeline

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-101 | Done | `TranscriptEvent` typed + stable key | Unit test identity/generation/attempt |
| TRN-102 | Done | `ActivityNormalizer` | Toàn corpus normalize, không drop |
| TRN-103 | Done | Lifecycle merger | start/delta/complete thành một item đúng |
| TRN-104 | Done | Sanitizer command/URL/tool payload | Secret corpus không lộ |
| TRN-105 | Done | Unknown fallback contract | Unknown fixture có generic event |
| TRN-106 | Done | Live/replay equivalence harness | Canonical comparisons xanh |

## Wave 2 — controller/model

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-201 | Done | Một canonical model cho conversation active | Không duplicate source/render state |
| TRN-202 | Done | Generation fencing khi switch conversation | Late delta bị loại |
| TRN-203 | Done | Pagination prepend theo stable key | Không reorder/duplicate, viewport giữ nguyên |
| TRN-204 | Done | Streaming coalescing một chỗ | Completion flush ngay, CPU bounded |
| TRN-205 | Done | Presentation grouper | Group có thể tái tạo, không đổi storage |
| TRN-206 | Done | Bounded detail/asset references | Không copy giant payload vào mọi role |

## Wave 3 — UI nền tảng

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-301 | Done | Virtualized viewport và two-state scroll | Shell/delegate tách ownership; native 4×36 frames, sai khác tối đa 2/255 |
| TRN-302 | Done | Turn/user/final hierarchy | Final tách activity; Working không overlay |
| TRN-303 | Done | Activity renderer registry | Family registry, typed mappings và intentional unknown fallback có regression |
| TRN-304 | Done | Generic activity detail | Type + sanitized payload mở được |
| TRN-305 | Done | Activity group expand/collapse | Keyboard/order/count; state theo scope/key giữ qua recreate, bounded 256 entries |
| TRN-306 | Doing | Token/icon/density/accessibility | Native light/dark 100/125/200%; QAccessible role/name/action và keyboard qua. Còn kiểm tra VoiceOver thủ công |

## Wave 4 — renderer parity

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-401 | Done | Command renderer | Exact command copy; fail/interrupted; 16 KiB output pages; evicted/5 MiB detail rehydrate từ session, hash/scope fenced |
| TRN-402 | Done | File renderer | Read/create/edit/delete/move; ordered files/destination/statistics và context actions |
| TRN-403 | Done | Search renderer | Web/code/file semantic riêng và safe detail; source thiếu schema dùng generic |
| TRN-404 | Done | Browser renderer | 5 operation, title/domain, safe URL, screenshot attachment; unknown không đoán operation |
| TRN-405 | Done | Image activity/gallery | `Viewed 4 images`, 1/4/50/broken, bounded decode/batch và preview; benchmark memory thuộc TRN-503 |
| TRN-406 | Done | MCP/dynamic/app renderer | Server/tool identity, sanitized detail, failed result và corpus replay |
| TRN-407 | Done | Approval/question/error renderer | Daemon replay đóng receipt, reject resubmit, expiry; lifecycle retry giữ attempt riêng; không tự suy diễn đáp án |
| TRN-408 | Done | Artifact/link/Mermaid/copy/export | Explicit safe resource open; secret answer redaction; copy/link/image/Mermaid regression giữ nguyên |

## Wave 5 — integration và cutover

| ID | Trạng thái | Việc | Acceptance |
|---|---|---|---|
| TRN-501 | Todo | Daemon restart/replay integration | Live và reopened transcript tương đương |
| TRN-502 | Todo | Remote/privacy integration | Không nới command output hoặc raw secret |
| TRN-503 | Todo | Performance/resource benchmark | Không O(total history); cache bounded |
| TRN-504 | Todo | Visual QA matrix | Screenshot light/dark/scales được duyệt |
| TRN-505 | Todo | Production cutover | Chỉ V2 sở hữu transcript state/view |
| TRN-506 | Todo | Xóa legacy pipeline | Không còn warm/cache/anchor state cũ |
| TRN-507 | Todo | Full suite + release DMG smoke test | Desktop + daemon hợp lệ trong bundle |
| TRN-508 | Todo | Cập nhật docs/handoff | Architecture và trạng thái khớp code |

## Checklist khi phát hiện capability mới

- [ ] Có raw fixture, không chỉ screenshot/text mô tả.
- [ ] Có dòng mới trong coverage ledger.
- [ ] Canonical subtype và lifecycle đã chốt.
- [ ] Sanitization/privacy đã đánh giá.
- [ ] Renderer riêng hoặc generic fallback có chủ ý.
- [ ] Live test.
- [ ] Replay/restart test.
- [ ] Grouping/order test nếu đi cùng activity khác.
- [ ] Keyboard/accessibility test nếu interactive.
- [ ] Performance budget nếu mang ảnh/output/payload lớn.
