# Transcript V2 — ma trận trạng thái tới cutover

Cập nhật: **2026-09-14**. Baseline code: `8d464fbc2fe9`, nhánh
`feat/worktree-base-and-delete`.

## Chú giải

| Ký hiệu | Nghĩa |
|---|---|
| 🟢 | Đã đạt gate và có bằng chứng test hoặc review tương ứng |
| 🟡 | Đang làm hoặc đã có một phần, nhưng chưa đủ điều kiện đóng gate |
| 🔴 | Chưa đạt; nếu nằm trên critical path thì đang chặn cutover |

Màu phản ánh **mức sẵn sàng cutover**, không chỉ phản ánh code đã tồn tại. Một renderer đã chạy nhưng
chưa qua live/replay, privacy, tải lớn hoặc accessibility vẫn là 🟡.

## Tổng quan theo mốc cutover

| Mốc | Trạng thái | Kết quả cần đạt | Điều còn thiếu để chuyển xanh |
|---|:---:|---|---|
| PR 0 — Inventory và characterization | 🟡 | Biết toàn bộ event/capability phải giữ | Session corpus thật, coverage ledger hoàn chỉnh, baseline visual/resource |
| PR 1 — Canonical event và normalizer | 🟢 | Một canonical projection không drop event | Gate unit/corpus đã đạt; giữ xanh bằng coverage test |
| PR 2 — Model/controller SSoT | 🟢 | Một model/ListView active; generation-fenced history; stable prepend; coalesced delta; bounded payload | Giữ xanh bằng regression switch/load older/burst payload |
| PR 3 — Transcript shell và registry | 🟡 | Shell/scroll/registry và interaction đã qua regression/native | Còn kiểm tra VoiceOver thủ công; keyboard và QAccessible đã qua |
| PR 4 — Renderer parity | 🟢 | Functional parity đạt acceptance Wave 4 | Load/remote/privacy toàn hệ thống vẫn là gate độc lập PR 5 |
| PR 5 — Persistence, remote, performance | 🔴 | Live = replay; privacy và resource có budget | Restart/daemon, remote privacy, benchmark và cache bounded |
| PR 6 — Production cutover | 🔴 | V2 là pipeline duy nhất và release dùng được | Flip seam, quan sát, xóa legacy, full QA và DMG smoke |

**Trạng thái chung: 🟡 Implementation đang hình thành; 🔴 chưa sẵn sàng cutover.**

## Ma trận chi tiết

### 1. Contract và data pipeline

| Gate | Backlog | Trạng thái | Bằng chứng hiện có | Việc còn lại |
|---|---|:---:|---|---|
| Phase `final_answer` / commentary rõ ràng | TRN-003 | 🟢 | Final và commentary có row riêng; reasoning bị ẩn; có regression test | Giữ contract khi nhập thêm corpus |
| Event inventory và coverage ledger | TRN-001 | 🟢 | Aggregate inventory từ daemon journal thật; registry test bao phủ mọi protocol/item type quan sát được | Giữ inventory/registry đồng bộ khi protocol đổi |
| Corpus từ session thật | TRN-002 | 🟢 | Journal inventory thật + canonical/protocol fixture đã loại nội dung người dùng | Các tải 1/4/50 ảnh và restart thuộc gate integration tương ứng |
| Canonical event + stable identity | TRN-101 | 🟢 | `TranscriptEvent` typed; key gồm session/generation/turn/item/attempt; identity tests qua | UI vẫn nhận projection map ở boundary, không dùng map làm source of truth mới |
| Normalizer không drop event | TRN-102, TRN-105 | 🟢 | `ActivityNormalizer`, coverage registry và visible generic fallback; toàn corpus test qua | Event mới phải thêm fixture trước khi merge |
| Lifecycle start/delta/complete/retry | TRN-103 | 🟢 | `TranscriptLifecycleMerger` cùng test completed/failed/retry và stable attempt | Controller sẽ sử dụng merger ở PR 2 |
| Sanitizer command/URL/tool payload | TRN-104 | 🟢 | Sanitizer chung cho nested keys, CLI option, Bearer credential, URL authority/query/fragment | Remote boundary vẫn phải có integration test riêng ở PR 5 |
| Live/replay canonical equivalence | TRN-106 | 🟢 | Toàn versioned corpus được feed theo live và replay rồi so canonical output | Restart giữa item running vẫn thuộc TRN-501 |

### 2. Model, conversation và streaming

| Gate | Backlog | Trạng thái | Bằng chứng hiện có | Việc còn lại |
|---|---|:---:|---|---|
| Một model cho conversation active | TRN-201 | 🟢 | Một `AgentMessageModel` và một transcript ListView; cache chỉ giữ anchor theo scope | Không tạo hidden delegate/model khi đổi conversation |
| Generation fencing khi đổi conversation | TRN-202 | 🟢 | Selection/request generation hủy callback history cũ; turn fence loại late delta | A → B → A và late-delta regressions qua |
| Pagination prepend theo stable key | TRN-203 | 🟢 | Model bỏ key trùng trong page/giữa page; history overlap và viewport regression qua | Stable key giữ nguyên sau prepend và stream tiếp |
| Streaming coalescing một chỗ | TRN-204 | 🟢 | Delta final batch 16 ms; completion flush đồng bộ; burst 100 delta chỉ phát update đã batch | Giới hạn tần suất ~60 update/s trên GUI thread |
| Presentation grouping tái tạo được | TRN-205 | 🟢 | `TranscriptPresentationGrouper` thuần, không mutate input; live/replay và deterministic tests qua | Adjacent image order/count giữ nguyên |
| Bounded detail/asset references | TRN-206 | 🟢 | Lazy ref >32 KiB; 64 entry/4 MiB; evicted và 5 MiB payload rehydrate từ session, hash/scope fenced | Transient parse và total history benchmark thuộc TRN-503 |

### 3. Transcript shell và interaction

| Gate | Backlog | Trạng thái | Bằng chứng hiện có | Việc còn lại |
|---|---|:---:|---|---|
| User/activity/final hierarchy | TRN-302 | 🟢 | Final tách khỏi activity; không còn `Working` overlay; behavioral tests qua | Giữ invariant khi tách shell mới |
| Two-state scroll | TRN-301 | 🟢 | Viewport `ListView`/`TranscriptMessage` tách ownership; native sau extraction 4×36 frames, max 2/255 | Legacy anchor cleanup thuộc TRN-506 |
| Renderer registry + fallback | TRN-303, TRN-304 | 🟢 | Family registry; typed operations; unknown giữ safe detail; question/error dùng message shell | Không suy đoán subtype mới |
| Group expand/collapse | TRN-305 | 🟢 | Keyboard/copy/preview; deterministic count/order; state giữ qua scope switch/recreate, bounded 256 | Full load benchmark thuộc TRN-503 |
| Token, density, icon và accessibility | TRN-306 | 🟡 | Native light/dark 100/125/200%; icon ảnh nhận token đúng; QAccessible Button/name và keyboard qua | VoiceOver thủ công chưa chạy; không đồng nhất API test với screen-reader QA |

### 4. Renderer parity

| Capability | Backlog | Trạng thái | Đã có | Để chuyển xanh |
|---|---|:---:|---|---|
| Command | TRN-401 | 🟢 | Exact copy, cwd/exit/duration; failed/interrupted text; 16 KiB output pages; evicted/5 MiB recovery; retry identity corpus | Performance benchmark PR 5 |
| File read/create/edit/delete/move | TRN-402 | 🟢 | Typed read/search; per-file operations, destination/stats, order/context actions và copy | Giữ fixtures khi provider đổi schema |
| Web/code/file search | TRN-403 | 🟢 | Web/code/file semantic riêng; safe result detail; unknown fallback | Không suy diễn operation từ text command |
| Browser open/navigate/click/download/screenshot | TRN-404 | 🟢 | 5 operation; page title/domain; sanitized URL; local screenshot; explicit safe open | Các tool chưa có schema giữ integration fallback |
| Image gallery | TRN-405 | 🟢 | 1/4/50 và broken-image tests; batch/decode bounded; keyboard mở preview gốc, Escape đóng | Measured memory budget PR 5 |
| MCP/dynamic/app/connector | TRN-406 | 🟢 | Server/tool identity; safe detail; result.isError thành failed; corpus live/replay | Unknown giữ generic detail có chủ ý |
| Approval/question/error/retry | TRN-407 | 🟢 | Daemon journal replay end-to-end, expiry/resubmit rejection; error/attempt lifecycle test | Resolution chỉ có ID hiển thị closed, không bịa đáp án |
| Artifact/link/Mermaid/copy/export | TRN-408 | 🟢 | Safe resource open; no auto-open; secret answer redaction; existing link/Mermaid/code/image/copy suites giữ xanh | Không thêm raw export hoặc thực thi từ replay |

### 5. Integration, quality và release

| Gate | Backlog | Trạng thái | Điều kiện xanh |
|---|---|:---:|---|
| Daemon restart/replay | TRN-501 | 🔴 | Đóng/mở giữa item running; live và reopened canonical transcript tương đương |
| Remote/privacy boundary | TRN-502 | 🔴 | Không lộ raw secret/command output ngoài policy ở row, detail, copy và remote |
| Performance/resource | TRN-503 | 🔴 | Baseline + budget cho 1k/10k rows, 50 delta/s, 1/4/50 ảnh, giant output |
| Visual QA | TRN-504 | 🔴 | Screenshot light/dark/scales cho final, groups, gallery, error, unknown và stream |
| Production seam sang V2 | TRN-505 | 🔴 | Chỉ V2 sở hữu transcript model/view/state trong session production |
| Xóa legacy pipeline | TRN-506 | 🔴 | Không còn warm view, anchor correction, duplicate cache hoặc giant transcript delegate |
| Full suite + release DMG smoke | TRN-507 | 🔴 | Full suite xanh; DMG mount; Desktop mở; daemon executable; smoke đủ file/command/browser/image |
| Architecture và handoff docs | TRN-508 | 🟡 | Matrix/backlog hiện đã phản ánh trạng thái; cập nhật tiếp khi từng gate đổi màu |

## Bằng chứng xanh gần nhất

| Kiểm tra | Kết quả |
|---|---|
| Full private / Agent test suite | 🟢 15/15 suites; Agent 67 passed, 0 failed, 1 live skip. Xem `evidence/2026-09-13-shell-renderers.md` |
| Nhóm `agent|chat` | 🟢 3/3 suites passed |
| Dev build | 🟢 Build và link KetPlus Desktop thành công |
| Whitespace validation | 🟢 `git diff --check` sạch tại lần kiểm gần nhất |
| Cutover corpus/restart/remote/performance/DMG | 🔴 Chưa có bằng chứng đạt gate |

Các test xanh hiện tại chứng minh lát cắt implementation không phá regression đã biết; chúng **không**
thay thế các gate cutover còn đỏ.

## Critical path tới cutover

| Thứ tự | Trạng thái | Công việc | Điều kiện chuyển bước |
|---:|:---:|---|---|
| 1 | 🟢 | Đóng event inventory và thu corpus thật | Inventory và sanitized corpus có expected canonical projection |
| 2 | 🟢 | Hoàn thiện typed normalizer, lifecycle và equivalence | Toàn corpus live/replay giống nhau; unknown không bị drop |
| 3 | 🟢 | Controller: generation, pagination, coalescing, bounded refs | Functional regressions qua; restart/load benchmark tổng thể ở bước 5 |
| 4 | 🟢 | Đóng functional renderer parity | Command/file/web/browser/image/integration/approval/error/artifact có policy và test |
| 5 | 🔴 | Chạy privacy, restart, remote, performance, visual và accessibility gates | Không còn blocker ở PR 5 |
| 6 | 🔴 | Flip production seam và quan sát | V2 là source of truth duy nhất; rollback seam vẫn kiểm soát được |
| 7 | 🔴 | Xóa legacy, chạy full regression và DMG smoke | Release chứa Desktop + daemon và smoke từ DMG đã mount thành công |

## Quy tắc đổi màu

- 🟡 → 🟢 chỉ khi acceptance trong `backlog.md` có test, screenshot, benchmark hoặc review link.
- 🔴 → 🟡 khi đã có implementation chạy được nhưng còn thiếu ít nhất một gate bắt buộc.
- Nếu regression, privacy leak hoặc live/replay mismatch xuất hiện, hạng mục liên quan quay về 🔴.
- Không cutover nếu generation fencing, replay, privacy, bounded resource, visual/a11y hoặc DMG smoke còn
  🟡/🔴.
- PR 6 chỉ được 🟢 khi mọi dòng critical path phía trên đều 🟢 và legacy pipeline đã bị xóa sau thời
  gian quan sát production seam.
