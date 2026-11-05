# Điều tra tải Desktop khi stream chat — phiên thực tế

Ngày: 2026-09-12. Trạng thái: **investigation only; chưa sửa runtime, chưa kết luận root cause duy nhất**.

Người dùng báo máy nặng khi assistant stream text, nhẹ lại khi stream kết thúc, và yêu cầu điều tra bằng chính dữ liệu phiên đang dùng. Sau điều tra, người dùng yêu cầu chỉ ghi tài liệu và mở PR để không xung đột với các team đang sửa core.

## Phạm vi và provenance

- Đọc journal SQLite đang chạy ở chế độ read-only; xác nhận đúng session bằng event chứa câu báo lỗi vừa gửi, không chỉ chọn session có trạng thái running.
- Lấy process CPU/RSS snapshots và hai stack samples của Desktop đang chạy (8 giây và 30 giây, sampling interval 10 ms). Sample dài bắt đầu khoảng 14:40:57, múi giờ +07:00; có cả streaming commentary và khoảng chờ tool, không phải continuous-stream benchmark.
- App quan sát: KetPlus 0.1.1, macOS 26.2, arm64; Desktop và daemon bundled đang chạy. Chưa xác minh exact source SHA của installed binary: version 0.1.1 không đủ chứng minh binary tương ứng một commit cụ thể.
- Source trace ban đầu tại local `15504e489820a4096dbaaff815e3e6d44b853dfa`; các đường cập nhật chính được đối chiếu lại tại base PR tài liệu `73bddc13aab3425636fbd2f0a61860d95d13402c` từ `origin/develop`.
- Không build, replay dữ liệu thật vào process khác, restart Desktop/daemon, gọi provider, hoặc sửa journal trong cuộc điều tra.
- Không commit raw transcript, session identifiers, absolute machine paths hoặc stack samples. Các số liệu dưới đây là aggregate tại thời điểm quan sát; journal tiếp tục tăng khi cuộc hội thoại tiếp diễn.
- Stack samples còn là local temporary artifacts, không phải durable CI evidence và có thể bị hệ điều hành dọn. Báo cáo này không cung cấp một benchmark có thể tái lập độc lập chỉ từ các con số tổng hợp.

## Quan sát thực tế

| Chỉ số | Giá trị quan sát | Giới hạn diễn giải |
| --- | --- | --- |
| Raw events của session | Khoảng 139.000 events; 68,6 MiB serialized payload | Không phải kích thước DB trên đĩa hoặc bộ nhớ UI |
| Compact replay của session | Khoảng 13.300 events; 31,9 MiB payload | Không đồng nghĩa tất cả đã được render/giữ trong view |
| Một assistant message ở turn gần đây | 7.977 UTF-8 bytes, 1.669 delta trong khoảng 58 giây | Đo từ journal, không từ UI frame trace |
| Peak delta rate của turn đó | 48 delta trong một bucket giây | Không phải FPS; thời điểm journal nhận có thể khác UI xử lý |
| Tổng kích thước body tích lũy của message trên | 6.537.206 bytes | Tổng prefix sau mỗi delta; không phải measured parser allocation hoặc CPU bytes |
| Desktop CPU snapshots | Khoảng 28–52% | Các snapshot, không phải trung bình benchmark; process CPU theo quy ước công cụ hệ thống |
| Daemon CPU snapshots | Khoảng 0–0,2% | Không loại trừ burst giữa các lần đo |
| Desktop physical footprint | Khoảng 1,4 GiB; peak 1,9 GiB | Toàn process, gồm các view khác; không chứng minh memory leak |

Trong cùng thời gian có build C++ của team khác, WindowServer tải cao và FSEvents có footprint lớn. Đây là confounders; không quy tất cả độ nặng của máy cho transcript.

Stack sample của installed Desktop có các nhánh `AgentMessageModel::set`, `QTextMarkdownImporter::import`, `formatMessageDocument`, text shaping/layout, cùng `GitService::refresh`, `setWorktrees` và `rebuildSkillModel`. Sự xuất hiện trong sample xác nhận công việc thực sự xảy ra; chưa xác định được tỷ trọng CPU hay độ dài stall của từng nhánh. Không cộng inclusive stack counts thành CPU percentages.

## Source trace và giả thuyết

### 1. Full-body update theo từng delta

[AgentSessionState.cpp](../../../modules/ai/src/agent/AgentSessionState.cpp), `appendAgentDelta()`, nối delta vào body rồi gọi `updateMessage()` với message đầy đủ. [AgentMessageModel.cpp](../../../modules/ai/src/agent/AgentMessageModel.cpp), `set()`, normalize và phát `dataChanged` cho roles thay đổi.

[AgentWorkspace.qml](../../../modules/ai/src/agent/qml/AgentWorkspace.qml) bind body vào `TextEdit` với `MarkdownText` cho assistant. `onTextChanged` gọi `updateParagraphFormat()`: format document, lấy code-block geometry và Mermaid placeholder positions. [AgentWorkspaceMedia.cpp](../../../modules/ai/src/agent/AgentWorkspaceMedia.cpp), `formatMessageDocument()`, duyệt blocks/fragments của document.

Với message 7.977 byte, việc cộng các prefix cho ra 6,54 MB, khoảng 820 lần body cuối. Điều này minh họa update amplification nếu xử lý mỗi prefix, **không chứng minh Qt đã parse đúng 6,54 MB hoặc xử lý đúng 1.669 UI updates**. Cần instrumentation để đếm batching/coalescing thực tế.

### 2. Layout và scroll restoration theo mỗi update

Trong `AgentWorkspace.qml`, `updateSessionMessage()` bọc `messages.set()` bằng begin/end update. `endTranscriptUpdate()` gọi restore ngay và thêm `Qt.callLater` restore. `restoreTranscriptPosition()` có guards nhưng khi chạy sẽ `forceLayout()` và xử lý follow-tail/anchor.

ListView đã có `reuseItems` và cache buffer: không phải hoàn toàn thiếu virtualization. Tuy nhiên một assistant message dài vẫn là một TextEdit nguyên khối; virtualization theo row không giới hạn chi phí parse/layout bên trong row đang stream.

### 3. Git/skills cạnh tranh GUI thread

[AgentWorkspaceView.cpp](../../../modules/ai/src/agent/AgentWorkspaceView.cpp), `setWorktrees()`, rebuild worktree/skills và emit active-path change mà không có early return cho cùng snapshot. [AgentWorkspaceSkills.cpp](../../../modules/ai/src/agent/AgentWorkspaceSkills.cpp), `rebuildSkillModel()`, quét đệ quy skill directories, đọc metadata và sort model.

Stack thực tế có nhánh worktreesChanged → setWorktrees → rebuildSkillModel trên main thread, cùng timer-triggered Git refresh. Đây là tải phụ có bằng chứng; chưa chứng minh chính streaming kích hoạt tất cả refresh hoặc skills scans.

## Chưa chứng minh

- Chưa có đối chứng synchronized streaming versus idle sau turn.completed với cùng viewport và workload ngoài app được giữ ổn định.
- Chưa đo GUI stall duration, native frame timings, số parser/layout calls, delegate churn hoặc memory riêng của conversation.
- Chưa replay final answer dài bằng timing từ journal trong isolated test; sample hiện tại chỉ bao gồm commentary ngắn và tool waits.
- Chưa xác nhận binary/source exact match; source analysis là cơ chế khả nghi được hỗ trợ bởi symbols trong stack, không phải toàn bộ build provenance.
- Chưa chứng minh memory leak, SQLite bottleneck hoặc một root cause duy nhất. Các snapshot hiện tại ưu tiên điều tra Desktop hơn daemon nhưng không loại trừ các bottleneck khác.

## Đề xuất xử lý cho các owner — chưa authorize implementation

| Owner | Công việc đề xuất | Điều kiện/kiểm chứng |
| --- | --- | --- |
| R05 + UI composition | Coalesce delta trước UI update; flush cuối turn; giảm forced layout/restore lặp | Giữ đủ text, final status, selection, anchor và Latest; đo số updates thực tế |
| R06 renderer | Cache blocks đã ổn định, chỉ parse/layout vùng thay đổi; bounded large-message handling | Không làm sai Markdown fences/lists/tables khi delta chia token; preserve links/code/Mermaid |
| Desktop skills/sidebar owner | Invalidate skills theo thay đổi thật; cache và tránh synchronous rescans không liên quan | Giữ discovery/enable state đúng khi skills hoặc workspace thay đổi |
| R03/R05, phối hợp R02 | Bounded history/model theo item và byte; continuation lossless | Contract-dependent paging chỉ triển khai sau schema freeze; không tự đổi IPC |
| Integration/R14 | Tách stream/render workload khỏi build/filesystem/Git background load | Baseline/candidate cùng binary provenance, viewport và trace methodology |

Không chuyển generic public Git fixes vào private repo, không sửa vendor/ketplus-cm tại chỗ. Nếu cần thay public CM, dùng public owner và release/pin gate. KetJS/KetAtlas vẫn read-only.

## Kế hoạch đo tiếp theo

1. Ghi exact binary/source provenance, active view và các background workloads; không terminate job team khác để làm đẹp kết quả.
2. Dùng local-only replay của session nếu được giao implementation/measurement scope riêng. Chỉ giữ event cần thiết, không commit raw text hoặc tool outputs; test fixture chia sẻ phải synthetic/sanitized.
3. Trace timestamps của delta receive, UI apply, parse/format/layout, scroll restoration, frame và turn completion; trace chỉ metadata/counts/durations.
4. So sánh streaming, paused delivery và completed/idle; phân biệt nội dung dài đang hiển thị với lịch sử ngoài viewport. Kiểm tra follow-tail, đọc phía trên, code block dài và switching.
5. Báo raw samples và phân phối với đủ sample count, không suy p95/p99 từ vài snapshot; kiểm tra bounded memory và lossless final text.

Không mở native capture khi chưa có quyền. Việc bật trace cho một app launch mới hoặc thay installed build phải được phối hợp riêng; PR tài liệu này không thực hiện các bước đó.

## Handoff

Đây là input bổ sung cho [kế hoạch refactor](../README.md), không thay thế proposal/backend freeze, không thay ownership register và không đánh dấu gate performance passed. PR chỉ thêm tài liệu độc lập để tránh conflict với core, workload generators và Integration manifests đang được các team cập nhật.
