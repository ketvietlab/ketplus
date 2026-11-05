# Transcript V2 — kế hoạch verification

## 1. Test pyramid

### A. Normalizer unit tests

- start/delta/complete hợp nhất đúng một item;
- sequence và identity ổn định;
- commentary hiển thị lossless; reasoning không rò vào transcript;
- final answer lossless;
- sanitizer command/URL/tool payload;
- unknown type tạo generic event;
- singular/plural và count cho file/image/group;
- lỗi và retry giữ attempt riêng.

### B. Model/controller tests

- live/replay equivalence;
- prepend history không duplicate hoặc đổi key;
- late delta của generation cũ bị loại;
- switch conversation không trộn data;
- restart giữ đúng turn/order/status;
- bounded window/cache dưới tải lớn;
- broken asset/output không làm mất cả activity.

### C. QML behavioral tests

- đúng renderer theo subtype và fallback;
- expand/collapse giữ đúng child order;
- exact command/path/URL detail copy được;
- gallery 1/4/50 ảnh và image preview;
- browser detail dùng safe origin và redacted URL;
- keyboard/focus/accessibility label;
- running/completed/failed có text, không chỉ màu.

### D. End-to-end

- app-server → daemon journal → Desktop live;
- đóng/mở app → replay giống live;
- load older → viewport không nhảy;
- remote client nhận projection đúng privacy policy;
- release DMG chứa daemon + Desktop và chạy được trên clean state.

## 2. Scenario matrix bắt buộc

| ID | Scenario | Kỳ vọng chính |
|---|---|---|
| TRN-QA-001 | Final answer có nhiều commentary trước đó | Commentary, activity và final đúng thứ tự; không có reasoning |
| TRN-QA-002 | Command success | Exact command, cwd, exit 0; output collapse |
| TRN-QA-003 | Command fail/interrupted | Trạng thái đúng, không biến thành completed |
| TRN-QA-004 | Read/edit/delete nhiều file | Group count đúng; expand đủ path đúng thứ tự |
| TRN-QA-005 | Viewed 1 image | Singular label, preview mở đúng ảnh |
| TRN-QA-006 | Viewed 4 images | Label 4; expand đúng bốn thumbnail |
| TRN-QA-007 | Viewed 50 images | UI/cache bounded; vẫn truy cập đủ 50 refs |
| TRN-QA-008 | Một ảnh hỏng | Placeholder/error; các ảnh khác vẫn hiện |
| TRN-QA-009 | Opened website | Page title + domain; click chủ động mới mở |
| TRN-QA-010 | URL có token/code/password | Không lộ secret ở row, detail, copy hoặc remote |
| TRN-QA-011 | Browser → editor → command | Chronological order đúng; group không làm mất item |
| TRN-QA-012 | Web search có sources | Query và sources đúng, không render như generic prose |
| TRN-QA-013 | MCP/dynamic/app tool | App/server/tool rõ ràng; safe detail mở được |
| TRN-QA-014 | Approval/question pending rồi trả lời | Interactive khi pending; immutable result khi replay |
| TRN-QA-015 | Unknown future item | Generic row visible với type/payload sanitized |
| TRN-QA-016 | Người dùng đang đọc khi stream | Top row/pixel offset không đổi |
| TRN-QA-017 | Người dùng ở cuối khi stream | Follow tail; không bị “Working” phủ nội dung |
| TRN-QA-018 | Jump to latest | Chủ động về cuối và tiếp tục follow |
| TRN-QA-019 | Restart giữa item đang chạy | Trạng thái recover hợp lý; không duplicate |
| TRN-QA-020 | Load older nhiều lần | Không duplicate/reorder; focus và viewport ổn định |
| TRN-QA-021 | Switch conversation nhanh | Late delta không lọt sang conversation khác |
| TRN-QA-022 | Giant final/command output | Bounded render; nội dung đầy đủ mở/copy được |
| TRN-QA-023 | Light/dark + scale 100/125/200% | Icon, text, spacing, thumbnail không lệch/cắt |
| TRN-QA-024 | Keyboard + screen reader | Mọi group/detail/gallery sử dụng được |

Không được xóa scenario khỏi matrix để làm suite xanh. Nếu sản phẩm đổi quyết định, cập nhật contract
và ghi lý do trước khi đổi expected behavior.

## 3. Coverage gate tự động

CI đọc corpus raw event và tạo tập `(type, subtype)`. Test so tập đó với coverage registry:

```text
observed event types - registered types = ∅
registered renderer or explicit generic policy = 100%
dropped normalized events = 0
```

Một event có `type` đã biết nhưng subtype mới vẫn phải làm test đỏ, trừ khi registry khai wildcard
có chủ ý và generic row đã được snapshot/test.

## 4. Replay equivalence gate

Với cùng một turn:

1. Feed từng notification theo thứ tự live.
2. Feed snapshot/history từ daemon sau completion.
3. So canonical event key, sequence, type, status, target và sanitized detail.
4. So visual group membership và group count.

Cho phép khác timestamp tạm thời hoặc animation state; không cho phép khác nội dung, thứ tự, trạng thái
hoặc số item.

## 5. Visual QA

Chụp screenshot cố định cho:

- turn chỉ có final answer;
- file + command group collapsed/expanded;
- browser + image gallery 4 ảnh;
- approval/error;
- unknown activity;
- streaming khi ở cuối và khi đang đọc phía trên;
- light/dark ở ba scale factor.

So screenshot ở đúng viewport và font bundle của release. Pixel diff là tín hiệu; người review vẫn
phải kiểm hierarchy, clipping, wrapping, focus ring và alignment icon/text.

## 6. Performance và resource QA

Đo trước/sau trên cùng máy và cùng fixture:

- time-to-first-visible-row khi mở conversation lạnh;
- frame time khi scroll 1k và 10k rows;
- CPU khi nhận 50 delta/s;
- peak/steady memory với 1, 4 và 50 ảnh;
- cost khi expand/collapse giant command output;
- memory sau chuyển qua lại 20 conversation.

Gate kiến trúc:

- work khi append/update không tỷ lệ với toàn bộ history;
- thumbnail và output cache có giới hạn đo được;
- không parse lại toàn bộ Markdown trên mỗi delta;
- rời conversation phải giải phóng visual delegates/cache ngoài budget.

Ngưỡng số cụ thể được ghi sau PR 0 từ baseline; không tự chọn con số chỉ để suite qua.

## 7. Release gate

Chỉ build/giao release khi:

- focused suite, full private suite và coverage gate xanh;
- visual QA không còn blocker;
- replay equivalence và privacy test xanh;
- DMG mount được, app mở được, daemon có trong bundle và executable;
- smoke test tạo turn gồm file, command, browser và ảnh thành công;
- `git diff --check` sạch và tài liệu/backlog phản ánh đúng trạng thái.
