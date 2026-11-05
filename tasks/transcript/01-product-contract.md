# Transcript V2 — contract sản phẩm và UX

## 1. Mục tiêu

Transcript là lịch sử kết quả có thể kiểm chứng, không phải bản ghi mọi câu chữ model đã phát ra trong
quá trình suy nghĩ. Nó phải trả lời nhanh bốn câu hỏi:

1. Người dùng đã yêu cầu gì?
2. Agent thực sự đã đọc, sửa, chạy, xem hoặc mở gì?
3. Thao tác nào đang chạy, thành công hoặc thất bại?
4. Câu trả lời cuối là gì?

## 2. Bốn loại nội dung

| Loại | Có trong transcript? | Quy tắc |
|---|---:|---|
| User prompt và attachment | Có | Giữ nguyên văn, ảnh/file mở được |
| Reasoning nội bộ | Không | Không render, không biến thành activity |
| Commentary/progress prose | Có | Hiện theo thứ tự thời gian; coalesce delta để không render từng token |
| Actual activity | Có | Structured row, giữ chi tiết chính xác |
| Final answer | Có | Một response riêng sau activity của turn |

Nếu upstream có trường `phase`, `channel` hoặc tương đương thì dùng nó làm nguồn phân loại. Nếu
`agentMessage` không phân biệt commentary và final, Desktop không được đoán bằng nội dung câu chữ.
Message có phase `commentary` và `final_answer` đều được publish bằng identity riêng. Reasoning nội
bộ vẫn không được expose. Nếu protocol cũ thiếu phase, giữ message như assistant prose thay vì drop;
không suy đoán reasoning bằng nội dung câu chữ.

## 3. Activity phải giữ chi tiết nào

### File

- Hành động: read, create, edit, delete, move hoặc search.
- Đường dẫn chính xác và worktree/cwd liên quan.
- Với edit: thống kê dòng nếu có; diff mở trong detail/review, không nhồi vào row.
- Gộp `Edited 7 files` được phép, nhưng expand phải liệt kê đủ bảy file.

### Command

- Dòng chính: `Ran: <exact command>`; khi đang chạy dùng `Running: ...`.
- Hiện cwd trong detail.
- Exit status, thời lượng và trạng thái interrupted/failed nếu nguồn có cung cấp.
- Output dài collapse và tải theo nhu cầu; command không được rút gọn trong dữ liệu canonical.
- Secret trong argument/env phải redact trước presentation và trước export không đặc quyền.

### Image

- Một ảnh: `Viewed an image`; nhiều ảnh: `Viewed 4 images`.
- Expand thành grid thumbnail; click mở preview ảnh gốc.
- Giữ asset ID/source, kích thước, mime type, tên/alt text nếu có.
- Không đưa bitmap lớn trực tiếp vào list model. Thumbnail phải cache có giới hạn.
- Ảnh người dùng đính kèm và ảnh agent đã xem là hai semantic khác nhau.

### Browser và web

- Phân biệt `Searched the web`, `Opened website`, `Navigated`, `Clicked`, `Downloaded` và
  `Captured screenshot` nếu nguồn đủ dữ liệu.
- Row chính dùng page title và safe origin/domain.
- Full URL nằm trong detail/copy, nhưng token, credential và query nhạy cảm phải được redact.
- Screenshot gắn với đúng browser activity, không biến thành attachment rời mất ngữ cảnh.

### App, connector và tool

- Hiện app/server/tool và động từ có nghĩa: `Used Dev.ketplus.editor`, `Queried Google Drive`.
- Payload input/output mở trong detail khi an toàn.
- Summary kết hợp như `Used editor and browser, edited files, ran commands` chỉ là group header
  dẫn xuất. Canonical events bên dưới vẫn độc lập.

### Approval, question và error

- Approval/question là interactive card khi còn chờ; sau khi trả lời chuyển thành immutable result.
- Error không bị thay bởi status chung “Stopped”; phải gắn với đúng activity gây lỗi.
- Retry tạo attempt mới có identity riêng, không ghi đè lịch sử attempt trước.

## 4. Grouping

Chỉ gộp các event liền kề, cùng turn và cùng semantic family. Không gộp qua final answer, approval,
error hoặc user prompt.

Ví dụ được phép:

- `Read 5 files`
- `Edited 3 files`
- `Viewed 4 images`
- `Ran 6 commands`

Mọi group phải:

- có số lượng đúng;
- mở/đóng bằng chuột và bàn phím;
- giữ chronological order khi mở;
- không thay canonical storage;
- tự mở khi có failure/approval cần chú ý.

## 5. Live state và scroll

- Activity đang chạy được thêm/cập nhật tại vị trí thời gian của nó.
- Status `Working` nằm ngoài luồng nội dung; không phủ lên response.
- Nếu viewport đang ở cuối, transcript được phép follow tail.
- Khi người dùng scroll lên, mọi delta giữ nguyên `{topRowKey, rowPixelOffset}`.
- Chỉ follow lại khi người dùng bấm `Jump to latest` hoặc chủ động cuộn xuống đáy.
- Completion không được chèn lại toàn bộ turn và làm nhảy viewport.

## 6. Density và hierarchy

- Activity row có visual weight thấp hơn final answer.
- Icon dùng nét, kích thước và màu từ design token; không dùng emoji hệ thống cho semantic icon.
- Dòng collapsed là một dòng; detail chỉ chiếm chiều cao khi mở.
- User prompt và final answer có khoảng cách turn rõ ràng, không bọc mọi thứ trong card nặng.
- Thumbnail ảnh dùng kích thước cố định/bounded để không làm layout nhảy khi decode.

## 7. Privacy và an toàn

- Canonical event giữ raw payload trong tầng được bảo vệ; presentation dùng payload đã sanitize.
- Redact credential trong command, env, URL, headers, tool arguments và output preview.
- Không tự mở URL hoặc file chỉ vì transcript được replay.
- Remote client chỉ nhận trường được protocol cho phép; không vì V2 mà nới command-output privacy.
- Copy/export mặc định dùng sanitized view; raw export cần policy riêng, không nằm trong scope V2.

## 8. Accessibility

- Mỗi row có accessible name gồm động từ, đối tượng và trạng thái.
- Group, detail và gallery điều khiển hoàn toàn bằng bàn phím.
- Thumbnail có alt/filename fallback.
- Không truyền trạng thái chỉ bằng màu; running/success/failure có text hoặc icon + accessible label.
- Focus không tự nhảy khi stream hoặc khi group được cập nhật.

## 9. Ngoài phạm vi

- Hiển thị chain-of-thought/reasoning nội bộ.
- Biến transcript thành terminal đầy đủ hoặc document editor.
- Tự động chạy lại command khi replay.
- Tải mọi output/ảnh kích thước lớn ngay khi mở conversation.
- Đảm bảo renderer riêng cho mọi tool tương lai; generic fallback là contract bắt buộc cho khoảng trống đó.
