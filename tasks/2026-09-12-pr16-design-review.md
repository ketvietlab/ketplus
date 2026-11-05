# PR #16 — review flow và tính khả thi triển khai

Ngày: 2026-09-12. Phạm vi: review, không sửa implementation, không đăng review lên GitHub.

- PR: https://github.com/ketvietlab/ketplus-desktop/pull/16
- Design HEAD: `3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5`.
- Base/native desktop đối chiếu: `4b1412112b2f45a1e59689658f3b54f08439b133`.
- Đầu vào transcript: [review kiến trúc trước đó](2026-09-12-transcript-architecture-review.md).

## Kết luận

Hướng bố cục khả thi với kiến trúc Qt hiện tại; không cần viết lại toàn bộ app bằng WebView.
Chưa nên coi PR này là spec flow hoàn chỉnh để implement nguyên trạng. Cần sửa các lỗi
context/draft và bổ sung lifecycle, history và connection recovery trước.

Các mức P1/P2 bên dưới đánh giá rủi ro khi lấy mock làm hợp đồng triển khai, không khẳng định
mock đang gây lỗi backend thật. Prototype chủ động dùng dữ liệu tổng hợp và không gọi API.

## Bằng chứng và giới hạn

- `discover`: nhận diện đúng bundle; 10 screen, 7 flow.
- `validate` và `audit --strict`: pass; 56 node, 65 edge, 41 file local, không warning.
- Syntax check: pass cho 11 JavaScript file trong `screens`.
- Năm probe source bằng Node VM xác nhận các đường URL/state nêu dưới; không dùng browser,
  không truy cập daemon/provider và không tiêu thụ quota.
- CI của PR tại thời điểm đọc báo build macOS/Ubuntu thành công. Không dùng kết quả đó
  làm bằng chứng acceptance cho design hoặc performance.
- Viewer local không được cấp quyền truy cập. Không thử đường vòng. Chưa xác minh visual,
  focus, clipping hoặc tương tác trong **Try this screen** ở lần review này.
- Theo skill KétAtlas, có checklist `atlas.progress.json` trong bản review worktree riêng.
  Mọi screen vẫn Unassessed về implementation; không đánh dấu mock là native đã Verified.
  Không thay đổi worktree/PR của team design.

## Findings cần xử lý

### D01 — P1: conversation chưa thực sự thuộc worktree trong flow mock

`conversationCatalog` có ba identity toàn cục. Chuyển từ develop sang feature/onboarding
giữ nguyên `conversation=startup`, state running và các conversation status. UI đổi tên
worktree nhưng vẫn trình bày cùng conversation. Đây là chỗ mâu thuẫn với câu mỗi worktree
có files/conversations/context độc lập trong màn Worktrees.

Còn một lỗi route cụ thể: khi đang ở feature/onboarding, mở Worktrees rồi chọn dòng develop,
dòng này truyền query rỗng nên kế thừa tree hiện tại, không quay về develop.

- [Conversation catalog và routing](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/conversations.js#L1).
- [Worktree develop link](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/workspace.js#L128).

Sửa spec: mỗi repository/worktree có selectedConversation riêng; conversation gắn session
identity bền vững, không dùng branch label làm identity. Fixture nên có ít nhất hai
worktree với hai conversation khác nhau mỗi worktree. Đổi worktree không đổi cwd/owner của
session đang chạy. Test đổi A → B → A giữ đúng lịch sử, draft, trạng thái và reading anchor.

### D02 — P1: mất draft/attachment khi mở model, permissions hoặc điều hướng

`navigateDialog()` đổi URL; composer mới luôn tạo textarea và attachment container rỗng.
Nội dung đang gõ không được lưu trước điều hướng. Việc Atlas giữ draft khi hide/show chat
không bao phủ mở dialog, chuyển conversation, đổi theme hoặc mở context pane.

- [Điều hướng dialog](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/actions.js#L23).
- [Composer rỗng](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/workspace.js#L1).

Sửa spec: lưu draft theo conversation; dialog không thay owner của draft. Text, attachment,
scope tag và vị trí đọc phải giữ khi thao tác phụ. Chỉ clear input đã được daemon nhận;
không clear nếu gửi thất bại. Native đã có draft theo composer scope, cần tránh regression.

### D03 — P1: trạng thái thực thi, restore và kết nối đang bị gộp

Flow nối `question → done` ngay khi Submit answer; reconnect Retry dùng chung `runDemo`
với gửi yêu cầu mới. `missing` loại bỏ trạng thái running nên không còn Stop. Trong khi đó,
native cho phép session tiếp tục chạy khi Desktop disconnect hoặc thư mục bị xóa.

Composer vẫn cho gõ khi running, nhưng chỉ có Stop và Enter bị bỏ qua, dù footer ghi Enter
để gửi. Native hiện có steering; mock chưa thể hiện gửi bổ sung vào turn đang chạy.

- [Question, retry, stop transitions](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/atlas.json#L173).
- [Composer state](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/workspace.js#L2).
- [Running Enter handler](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/actions.js#L472).

Sửa spec:

- Tách connection state, history-load state, worktree availability và turn state.
- Answer → submitting → working; completed chỉ khi nhận turn completion. Có submit lỗi,
  nhiều câu hỏi, secret/free-form, pending request đã được client khác xử lý.
- Stop → stopping → interrupted/error; không hiện thành công chỉ vì đã click.
- Reconnect → lấy lại trạng thái/replay → tiếp tục theo dõi cùng turn; không tự gửi lại prompt.
- Worktree missing: lịch sử read-only, vẫn Stop được nếu turn đang chạy; không hứa chuyển
  conversation sang worktree khác. Nếu muốn di chuyển/fork thì phải là hành động riêng.
- Thể hiện steering đang gửi/đã nhận/lỗi. Approval là surface riêng nếu bật capability;
  không coi question card hay permission selector là approval UI.

### D04 — P1: thiếu flow đọc transcript lớn và các điều kiện performance

Workspace hiện có một đoạn transcript mẫu ngắn và một restore logo. Chưa có state cho
loading older, retry trang cũ, nội dung mới khi đang đọc phía trên, Latest, partial page
hoặc lịch sử cần Mac online. Vì vậy chưa thể dùng mock này để nghiệm thu yêu cầu transcript.

- [Transcript mock](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/workspace.js#L28).

Bổ sung journey riêng:

1. Mở conversation: hiển thị phần gần nhất của ba turn, bounded theo cả item/bytes.
2. Kéo lên: giữ item anchor và offset, chèn trang cũ; loading/error chỉ ở đầu danh sách.
3. Live vẫn chạy: không kéo người dùng xuống; có new-content indicator và Latest.
4. Latest: quay về live tail và bật lại follow mode.
5. Reconnect/page overlap: không mất/trùng/đổi thứ tự item.
6. Partial turn và cloud hết retention: phân biệt chưa tải, không còn trên cloud và Mac offline;
   không trình bày như history đã kết thúc.

Command cần fixture một dòng với lifecycle Run/Ran/failed/interrupted. Giữ yêu cầu mobile
chỉ nhận lệnh, không command output; không vô tình đưa output lên cloud để phục vụ card mới.
Desktop local detail và future remote detail là các khả năng riêng.

Rich content cần loading/error/lazy state cho code, diff, table, Mermaid/media; không chỉ
đưa một payload lớn vào một delegate. Budget dữ liệu/parse cache và render window là hai
việc khác nhau. Ba turn vẫn có thể nhiều MiB.

### D05 — P1: Connections cần khớp auth, revoke và cam kết dữ liệu

Copy `Conversation history stays on your computer` dễ được hiểu rằng nội dung không rời Mac,
trong khi ADR hybrid đã chốt cloud hot replica tối đa 24 giờ và mobile có cache.

Mock có các regression/điểm không nhất quán:

- Chỉ Pixel mới pair có Revoke. iPhone cũ chỉ Details, dialog Details cũng không có Revoke,
  trong khi footer nói revoke bất kỳ lúc nào.
- Đăng xuất, chuyển trang khác rồi quay lại Connections sẽ về ready vì auth state không
  được giữ. Đây là lỗi mô phỏng state, không phải bằng chứng bypass backend thật.
- Form chỉ nhận account chứa `@`; contract phase này dùng KetSuite username/password,
  native chỉ kiểm tra nonempty. Không nên tự giới hạn thành email-only.
- Chưa thể hiện submitting/failure khi confirm/revoke, token hết hạn và sign-out effects.
  Close/Decline cần gắn với hủy pairing ticket như native, không chỉ đóng UI.

- [Connections copy, device rows và dialogs](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/connections.js#L1).
- [Email-only demo validation](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/actions.js#L296).
- [Hybrid decision](https://github.com/ketvietlab/ketplus-desktop/blob/4b1412112b2f45a1e59689658f3b54f08439b133/docs/decisions/0004-canonical-transcript-hybrid-retention.md).

Copy nên nói lịch sử đầy đủ lưu trên Mac; phần gần nhất được đồng bộ để dùng trên thiết bị
đã kết nối. Chi tiết retention nằm ở disclosure/help, không bắt user hiểu daemon/relay.
Mọi thiết bị đã pair phải thu hồi được; chỉ báo revoked sau acknowledgment thành công.
Cam kết mã hóa phải khớp contract/security verification riêng, không suy từ icon shield.

### D06 — P2: Atlas library chưa giữ identity xuyên suốt journey

Card truyền `project`, nhưng `K.url()` không giữ key này cho bước tiếp theo. Probe chọn
KetPlus Mobile rồi mở preview làm mất project; phần builder chat còn hard-code Onboarding.
Với nhiều Atlas thật, flow hiện không chứng minh được người dùng đang sửa/preview đúng bundle.

- [URL context allowlist](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/core.js#L25).
- [Library, builder và preview](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/atlas.js#L17).

Sửa spec: giữ bundle identity/path + worktree + conversation + flow ID qua library → map
→ try screen → continue designing. Display title không phải identity. Cần trạng thái bundle
bị đổi tên/xóa/invalid và chọn lại chính xác bundle. Preview valid không đồng nghĩa AI turn
đã completed; cần phân biệt map khả dụng và agent còn đang cập nhật.

### D07 — P2: `@Flow` là context, chưa phải bảo đảm cô lập chỉnh sửa

Điểm tốt là scope tag độc lập với flow đang xem và được ghi lại trong request receipt.
Tuy nhiên, UI nói `Limit your request to one flow`, còn response nói flow còn lại không đổi.
Một screen/CSS có thể dùng chung ở nhiều flow nên scope theo prompt không thể bảo đảm điều đó.
DESIGN.md đã ghi không bảo đảm provider behavior, nhưng UI copy vẫn tạo kỳ vọng mạnh hơn.

- [Flow scope copy](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/atlas-flows.js#L24).
- [Builder response](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/atlas.js#L58).

Chọn một contract rõ: tag là ngữ cảnh ưu tiên và UI nói đúng giới hạn; hoặc strict scope cần
dependency impact check, validate candidate và thông báo/xác nhận thay đổi shared assets.
Giữ last-known-valid preview khi AI đang ghi bundle; invalid intermediate JSON không được
làm mất map đang dùng. Không tự coi receipt demo là kết quả kiểm chứng thay đổi.

## Các điểm nhỏ nhưng cần đưa về đúng quyết định đã chốt

- [Connections dùng icon monitor](https://github.com/ketvietlab/ketplus-desktop/blob/3abe5c4e02418fd7e81ddecabd7cd57bc46d18a5/ketplus.ketatlas/screens/shell.js#L6),
  khác yêu cầu Lucide `unplug` trước đó.
- Signed-out screen đi thẳng vào form lớn, khác quyết định chỉ có nút Đăng nhập trước khi
  mở form. Nếu chủ động đổi UX thì ghi rõ thay đổi, không mặc định xem đã được duyệt.
- Code review đúng phạm vi read-only Git; không cần bổ sung stage/commit GUI ngoài scope.
  Tab lifecycle đã được đánh dấu ngoài độ sâu mock nhưng implementation không được làm mất
  unsaved buffers, terminal session/cwd hoặc file target khi điều hướng.
- Tạo worktree cần thêm đang tạo, lỗi Git/IO và retry có kiểm tra kết quả trước đó. Branch
  validation sample không thay được Git validation/runtime error.

## Mapping sang implementation thật

| Design | Nền tảng / owner | Đánh giá |
| --- | --- | --- |
| Sidebar, worktree/conversation selection | Private `apps/desktop` + `modules/ai` | Khả thi; reuse registry/session identity, không bind theo display label |
| Transcript/composer/context pane | `AgentWorkspaceView`, model và QML ListView | Giữ native virtualization/reading-anchor; redesign delegate theo canonical read model |
| Atlas map + floating chat | `AtlasBuilderTab`, `AtlasTab`, KetAtlas viewer | Layout overlay đã có; thêm stable bundle/flow context và bridge public có version |
| Atlas thumbnails | `AtlasIndex` + library model/cache | Cần bounded/lazy thumbnail generation; không mở WebView cho toàn bộ thư viện |
| Connections | `RemoteControlView` + daemon IPC/control plane | Sign-in/pair/approve/revoke đã có; map trạng thái thật và acknowledgment, không mang demo bypass sang Release |
| Editor/Git/terminal/settings/shared UI | Public KetPlus CM, rồi release/pin vào Desktop | Không sửa trong private vendor submodule hoặc tạo bản sao private |

Không port DOM mock thành toàn bộ app. Dùng Két Design System làm nguồn token/interaction,
mapping qua native theme/components. Viewer Atlas vẫn do KetAtlas cung cấp; thay đổi public
viewer/API cần làm đúng repo, không giả lập một viewer riêng trong Desktop.

## Performance acceptance cần có trước khi bắt tay redesign transcript

- Fixture 1.000/10.000 message, một message khoảng 1 MiB, turn vượt một response, nhiều tool.
- Streaming + đọc phía trên + prepend + resize/font change + Latest, không mất anchor.
- Đổi qua 20 conversation nhiều vòng; đo RAM plateau, warm switch và first-content latency.
- Decode/projection/SQLite/crypto/parse nặng ngoài UI thread; cache theo item revision.
- Chỉ mount visible/overscan delegates; lazy rich content và bounded data window riêng.
- Atlas nhiều node/card: thumbnail theo nhu cầu, giới hạn concurrent preview, giải phóng
  inactive WebView; không dùng hai iframe mẫu làm bằng chứng khả năng scale.
- UI audit 52 state không phải stress test. Cần native desktop baseline; iOS performance
  phải đo thiết bị thật trong task mobile riêng.

## Đề xuất handoff

1. Team design sửa D01–D03, D05–D07 và các regression nhỏ ngay trong bundle.
2. Thêm journey transcript ở D04, cộng async error/recovery states cần thiết.
3. Review tương tác trong viewer được cấp quyền, rồi chốt acceptance theo repo/module.
4. Lập task canonical correctness/paging/performance trước hoặc làm nền cho rich renderer;
   không coi thay layout là đã xử lý các gap T01–T10 của review trước.

Đề xuất review: **cần sửa flow trước khi duyệt làm spec triển khai**. Không phản đối hướng
bố cục, không yêu cầu thiết kế lại từ đầu. Chưa thực hiện build, deploy, commit, push hoặc merge.
