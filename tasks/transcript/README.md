# Transcript V2 — kế hoạch làm lại

Ngày lập: **2026-09-13**.

Đây là bộ kế hoạch thay thế phần transcript của KetPlus Desktop bằng một bề mặt giống Codex: chỉ
hiện yêu cầu của người dùng, các thao tác thực sự đã làm và câu trả lời cuối. Những đoạn tự sự trung
gian như “mình đang kiểm tra…” hoặc reasoning không nằm trong transcript.

## Baseline

| Thành phần | Baseline khi lập kế hoạch |
|---|---|
| Kho triển khai `ketplus-desktop` | `8d464fbc2fe9`, nhánh `feat/worktree-base-and-delete` |
| Phạm vi giữ nguyên | daemon journal, transcript window, protocol, encryption và approval |
| Phạm vi thay thế | projection/normalization trên Desktop, model trình bày và transcript QML |

Baseline chỉ để truy vết. Trước khi code phải audit lại event thực tế ở commit mục tiêu; không được
coi danh sách event trong tài liệu này là danh sách đóng.

## Contract sản phẩm không được phá

Một turn có ba lớp nhìn thấy:

```text
Yêu cầu của người dùng
  Hoạt động đã thực hiện
    Read AgentWorkspace.qml
    Edited ActivityBar.cpp
    Ran: cmake --build ...
    Viewed 4 images
    Opened developer.apple.com
  Câu trả lời cuối
```

- Hiện commentary/progress prose theo thứ tự; chỉ ẩn reasoning nội bộ.
- Không ẩn thao tác thực tế: file, command, browser, ảnh, search, app/connector, approval và lỗi.
- Dòng gộp chỉ là presentation. Mở rộng phải thấy đủ item gốc theo đúng thứ tự.
- Command phải giữ nguyên command và working directory; output dài được thu gọn.
- Đường dẫn, URL và payload phải che secret nhưng không được biến mất hoàn toàn.
- Event chưa biết phải hiện bằng generic fallback. **Không có nhánh `else continue` làm mất dữ liệu.**
- Câu trả lời cuối là nội dung riêng, không trộn với activity và không bị dòng “Working” đè lên.
- Streaming không tự kéo người dùng khỏi vị trí đang đọc.
- Replay sau khi mở lại app phải cho cùng thứ tự, grouping và nội dung như lúc live.

Chi tiết contract: [`01-product-contract.md`](./01-product-contract.md).

## Bộ tài liệu

| Tài liệu | Câu hỏi nó trả lời |
|---|---|
| [`01-product-contract.md`](./01-product-contract.md) | Transcript được phép và không được phép hiển thị gì? |
| [`02-event-coverage.md`](./02-event-coverage.md) | Mỗi event phải được giữ, chuẩn hóa và render thế nào? |
| [`03-implementation-plan.md`](./03-implementation-plan.md) | Thay pipeline cũ bằng pipeline mới qua các PR nào? |
| [`04-verification-plan.md`](./04-verification-plan.md) | Test gì để không tái phát và không miss chức năng? |
| [`05-cutover-status-matrix.md`](./05-cutover-status-matrix.md) | Hiện đang ở đâu, gate nào chặn cutover và thứ tự đóng gate? |
| [`backlog.md`](./backlog.md) | Danh sách công việc có ID, thứ tự và gate hoàn tất |

## KetAtlas review captures

- [`Transcript activity · light`](./evidence/ketatlas-transcript-actions-light.jpg)
- [`Final answer · light`](./evidence/ketatlas-transcript-answer-light.jpg)
- [`Viewed 4 images · dark`](./evidence/ketatlas-transcript-images-dark.jpg)

## Thứ tự thực hiện

1. Chụp inventory từ schema/protocol, fixture và session thật; cập nhật coverage ledger.
2. Chốt canonical `TranscriptEvent` và quy tắc phân loại final/commentary/activity.
3. Viết normalizer cùng generic fallback trước khi viết UI chuyên biệt.
4. Viết activity registry và các renderer nền tảng.
5. Nối live stream, replay, pagination và scroll contract.
6. Chạy coverage, behavioral, restart/replay, remote và visual QA.
7. Cut over rồi mới xóa pipeline cũ.

## Definition of done

Transcript V2 chỉ hoàn tất khi đồng thời đạt các điều kiện sau:

- Coverage ledger có mapping cho **100% event type/subtype quan sát được** từ app-server và fixture.
- Fixture `unknown future activity` vẫn tạo một row có thể mở payload.
- Có renderer riêng cho file, command, search, browser, image gallery, app/connector, approval và error.
- `Viewed 4 images` mở được đủ bốn ảnh; `Opened website` mở được chi tiết URL đã redact.
- Live và replay tạo cùng canonical items và cùng visual groups.
- Commentary live/replay lossless; reasoning nội bộ không xuất hiện.
- User đang đọc phía trên không bị kéo theo stream; `Jump to latest` là hành động chủ động.
- Test restart, pagination, remote/daemon, dark/light theme, keyboard và screen reader đều qua.
- Pipeline cũ chỉ bị xóa sau khi production seam chuyển sang V2 và bộ characterization test vẫn xanh.

## Kỷ luật chống bỏ sót

Mỗi khi Codex/app-server thêm event mới:

1. Thêm fixture thô vào corpus.
2. Ghi một dòng trong coverage ledger.
3. Chọn renderer chuyên biệt hoặc xác nhận generic fallback là chủ ý.
4. Thêm test live và replay.
5. Chỉ sau đó mới đánh dấu hỗ trợ hoàn tất.

Không duyệt PR nếu event fixture mới bị drop, nếu normalizer mất payload gốc, hoặc nếu UI chỉ đúng ở
live nhưng sai sau restart.
