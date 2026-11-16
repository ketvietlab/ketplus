# Audit daemon, danh tính và quyền agent — T-1

Ngày rà soát: 2026-11-16. Baseline public: `7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`
(HEAD của nhánh audit và `integration` khi nhận task).

**Đề nghị giữ release để reviewer xác minh DA-01 và DA-02.** Có hai phát hiện
mức High bằng phân tích mã và một sai lệch tài liệu mức Informational. Đây là
báo cáo điều tra, không phải xác nhận đã khai thác, bản sửa lỗi hay chứng nhận
an toàn toàn sản phẩm. Không sửa mã sản phẩm trong T-1.

## Phạm vi và mô hình tin cậy

Đã đọc `AGENTS.md` và các phần liên quan của `project-status.md`,
`repository-boundaries.md`, `architecture.md`, `daemon-architecture.md`,
`agent-workspace.md`, `development.md`, `testing.md`, `product-architecture.md`,
`licensing.md`, ADR 0034 về device pins và ADR 0036 về challenge v2.

- Chỉ rà mã public trong `apps/daemon`, `modules/remote`, `modules/ai` và fixture
  liên quan. Không đọc kho credential thật, transcript hay cấu hình tài khoản.
- Theo ADR 0034, relay có thể chủ động độc hại; danh sách thiết bị từ relay
  không tự cấp quyền. Pin do người dùng duyệt trên máy mới là căn cứ tin cậy,
  ngoại trừ một lần chuyển đổi enrollment cũ được ADR mô tả rõ.
- Token điều khiển local là quyền cấp người dùng OS. Token MCP agent chỉ cấp
  thao tác agent, định danh theo hội thoại/generation. Đây không phải sandbox
  ngăn mọi tiến trình cùng OS user đọc dữ liệu của nhau.
- Quyền file/shell phụ thuộc adapter và sandbox provider. Tên profile UI không
  chứng minh mọi provider có cùng mức cô lập. Browser/computer được inject có
  chính sách riêng; không coi miễn hỏi từng call là lỗ hổng tự thân.
- Hosted authorization, tenant membership, challenge redemption một lần và
  triển khai dịch vụ nằm ngoài bằng chứng của repository này.

## Coverage đã kiểm tra

Các dòng dưới đây là entry point đã lần theo, không có nghĩa đọc toàn bộ module.
Số dòng và kết luận gắn với baseline trên.

| Biên | Bằng chứng mã | Kết quả / giới hạn |
| --- | --- | --- |
| Local IPC admission | [LocalIpcServer.cpp:50](../../apps/daemon/src/ipc/LocalIpcServer.cpp#L50), `:241–355`, `:584–593`; [DaemonCredentialFile.cpp:16](../../modules/remote/src/DaemonCredentialFile.cpp#L16) | Socket `UserAccessOption`, hello bắt buộc, token control được so toàn byte; token agent đi nhánh riêng. File token Unix mở no-follow/nonblocking, kiểm owner/type/mode trên descriptor và giới hạn 4 KiB. Không kiểm chứng ACL Windows trong lượt này. |
| Agent identity và phạm vi | [AgentBroker.cpp:136](../../apps/daemon/src/agents/AgentBroker.cpp#L136), `:155–194`; [SessionHostAgents.cpp:43](../../apps/daemon/src/runtime/SessionHostAgents.cpp#L43); [AgentMcpServer.cpp:389](../../apps/daemon/src/agents/AgentMcpServer.cpp#L389) | Token ngẫu nhiên ánh xạ session/generation; IPC chỉ cho tập thao tác agent; broker kiểm generation hiện tại trước dispatch. Team tool còn qua lớp role riêng; chưa audit toàn bộ role matrix/team workflow. |
| Approval và cô lập hội thoại | [SessionHost.cpp:2001](../../apps/daemon/src/runtime/SessionHost.cpp#L2001), `:2064–2078`; [ApprovalBroker.cpp:202](../../apps/daemon/src/runtime/ApprovalBroker.cpp#L202); [AcpPermissions.cpp:12](../../apps/daemon/src/providers/acp/AcpPermissions.cpp#L12), `:57–78` | Approval gắn session/generation/kind, chặn giải quyết lần hai. ACP kiểm turn hiện hành và chỉ chọn `allow_once`/`reject_once`, không nâng thành always. Timer expiry được rà ở mã; không chạy fixture Qt. |
| UI client nhận event / đổi quyền | [CodexAppServerDaemonEvents.cpp:32](../../modules/ai/src/agent/CodexAppServerDaemonEvents.cpp#L32), `:95–108`; [AgentSessionConfiguration.cpp:63](../../modules/ai/src/agent/AgentSessionConfiguration.cpp#L63) | Client định tuyến theo session, bỏ generation lệch/cursor cũ; đổi model không tự ghi permission. UI không thay thế kiểm tra daemon. Không audit mọi callback UI. |
| Provider policy | [SessionHostTurns.cpp:33](../../apps/daemon/src/runtime/SessionHostTurns.cpp#L33); [ClaudeAdapter.cpp:238](../../apps/daemon/src/providers/ClaudeAdapter.cpp#L238); [CursorAdapter.cpp:255](../../apps/daemon/src/providers/CursorAdapter.cpp#L255); [review-guard.mjs](../../apps/daemon/providers/claude-sidecar/review-guard.mjs) | Codex map sandbox và approval policy; Claude map plan/acceptEdits/bypass; Cursor map ask/agent. Sidecar review read/verify kiểm bằng 6 test fake SDK. Không suy ra SDK thực hoặc CLI bên ngoài đã thực thi sandbox đúng. |
| Browser sign-in / challenge | [BrowserSignIn.cpp:58](../../modules/remote/src/cloud/BrowserSignIn.cpp#L58), `:94–133`; [CloudControlSignIn.cpp:33](../../apps/daemon/src/cloud/CloudControlSignIn.cpp#L33), `:124–235`; [CloudControlCredentials.cpp:25](../../apps/daemon/src/cloud/CloudControlCredentials.cpp#L25) | Loopback/state, PKCE handoff, kiểm trang trả về cùng origin, epoch ở sign-in/challenge. Client yêu cầu challenge v2; không kết luận dịch vụ đã triển khai đúng one-time redemption. |
| Remote identity / sign-out | [DevicePins.cpp:69](../../apps/daemon/src/cloud/DevicePins.cpp#L69); [CloudControlClient.cpp:367](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367), `:620–770`; [CloudControlSync.cpp:447](../../apps/daemon/src/cloud/CloudControlSync.cpp#L447); [CloudControlCommands.cpp:249](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L249) | Kiểm digest trước pairing và pin key; phát hiện DA-01/02 tại lifecycle callback/cache. Không suy rộng epoch guard ở heartbeat sang mọi callback. |
| Remote client / peer grant | [ControlV2Authentication.cpp:30](../../modules/remote/src/control/ControlV2Authentication.cpp#L30), `:229–267`; [RemotePeers.cpp:76](../../apps/daemon/src/transport/RemotePeers.cpp#L76) | Desktop đổi credential/sign-out tăng epoch, xóa signer cache và làm rỗng host pins. Peer check grant, capability, machine/workspace và device; không audit toàn bộ WebRTC stack. |
| Khóa và lưu trữ | [CloudCrypto.cpp:537](../../modules/remote/src/cloud/CloudCrypto.cpp#L537), `:661–675`; [SessionCredentials.cpp:78](../../apps/daemon/src/cloud/SessionCredentials.cpp#L78); [CredentialStore.cpp:22](../../modules/remote/src/cloud/CredentialStore.cpp#L22), `:94–146`, `:185–205` | Wrap dùng X25519/HKDF/AES-GCM và chữ ký; đổi workspace cất riêng content keys; lưu credential thực tế là file owner-only, không phải OS keychain (DA-03). Không có đánh giá mật mã độc lập. |

## DA-01 — Callback phân phối khóa sống qua sign-out và mở lại adoption

**Mức độ: High — confidentiality, vi phạm biên relay không đáng tin.**
Trạng thái: xác nhận control/data flow bằng phân tích mã; chưa chạy tái hiện C++.

**Mong đợi:** Sau sign-out, phản hồi phát sinh ở epoch cũ không được ghi pins,
tạo/distribute content keys hoặc phục hồi trust. Adoption chỉ dành cho enrollment
cũ thật sự, không dành cho một pin set vừa bị sign-out xóa.

**Thực tế và chuỗi bằng chứng:**

1. [CloudControlSync.cpp:454–465](../../apps/daemon/src/cloud/CloudControlSync.cpp#L454)
   chỉ kiểm `machineId_`/`daemonToken_` trước khi gửi request. Callback capture
   `this`, không capture/so `accountEpoch_`, machine hay workspace.
2. [CloudControlClient.cpp:367–410](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367)
   tăng epoch, xóa enrollment và `device-pins`, làm rỗng machine/token. Content
   keys và identity signing key không thuộc tập bị xóa. Hàm
   [request:272–332](../../apps/daemon/src/cloud/CloudControlClient.cpp#L272)
   vẫn gọi callback của reply; sign-out không hủy toàn bộ request đang bay.
3. Khi device-list reply đến sau sign-out, `trustedDevices()` gọi `pins_.adopt()`
   ([CloudControlSync.cpp:447–451](../../apps/daemon/src/cloud/CloudControlSync.cpp#L447)).
   [DevicePins.cpp:69–94](../../apps/daemon/src/cloud/DevicePins.cpp#L69) coi pin set
   vắng là chưa initialized và ghi các thiết bị active có public keys hợp lệ từ
   danh sách thành trusted. Không kiểm enrollment/epoch tại đây.
4. Callback tiếp tục duyệt journal, wrap các session chưa bị refused, ghi cache
   và POST envelopes ([CloudControlSync.cpp:469–519](../../apps/daemon/src/cloud/CloudControlSync.cpp#L469)).
   `wrapSessionKey()` vẫn lấy content key và ký; `validWrappedSessionKey()` không
   yêu cầu `senderDeviceId` khác rỗng
   ([CloudCrypto.cpp:569–610](../../modules/remote/src/cloud/CloudCrypto.cpp#L569),
   [:661–675](../../modules/remote/src/cloud/CloudCrypto.cpp#L661)).

**Điều kiện/tác động:** Daemon còn sống, có session/content key, một request danh
sách thiết bị đang chờ, sign-out thành công trước reply, và credential store vẫn
ghi được. Relay độc hại theo threat model có thể trả public keys do nó sở hữu.
Khi đó daemon tạo wrap cho thiết bị chưa được duyệt và gửi body cho relay. POST
sau sign-out có thể không có bearer; việc dịch vụ hợp lệ từ chối POST không bảo vệ
body trước chính relay độc hại đã nhận nó. Giới hạn 500 envelopes mỗi lượt vẫn
áp dụng. Không cần và không giả định sign-in tiếp theo để có luồng lộ khóa này.

**Hướng sửa / acceptance cho task sửa:** Fence callback trước mọi mutation bằng
epoch + machine/workspace đã capture, từ chối khi không enrolled; làm adoption
phụ thuộc trạng thái migration rõ ràng thay vì chỉ thiếu pins. Áp dụng quy tắc
tương tự cho callback approve/revoke pairing và fallback sender-key adoption.
Thêm fixture fake transport giữ reply, sign-out, rồi trả device list giả: assert
không có pin mới, wrap mới, refresh devices hay request upload khóa. Chạy thêm
case sign-out → enrollment khác → reply cũ. Không cần provider/network thật.

## DA-02 — Cache sender keys bỏ qua pin reset sau đăng nhập lại

**Mức độ: High — authorization, có điều kiện về thiết bị từng được tin cậy.**
Trạng thái: xác nhận cache invalidation thiếu bằng phân tích mã; chưa tái hiện
thực thi command end-to-end.

**Mong đợi:** Enrollment mới bắt đầu với pin set rỗng; khóa đã được chấp nhận
trước sign-out không còn là bằng chứng thiết bị được paired ở enrollment mới.

**Thực tế và bằng chứng:**

- [CloudControlCommands.cpp:249–258](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L249)
  trả cache hit ngay, trước `pins_.trustsKeys()`. Cache key chỉ là device/version;
  TTL là 10 phút ([CloudControlClient.h:62](../../apps/daemon/src/cloud/CloudControlClient.h#L62)).
- [Sign-out:367–410](../../apps/daemon/src/cloud/CloudControlClient.cpp#L367) không
  xóa `senderKeys_`. [Enrollment:198–234](../../apps/daemon/src/cloud/CloudControlSignIn.cpp#L198)
  reset pins nhưng cũng không xóa hoặc đổi namespace cache. `accountEpoch_` không
  được xét trong cache hit. Ngược lại, revoke-device có xóa cache
  ([CloudControlClient.cpp:756](../../apps/daemon/src/cloud/CloudControlClient.cpp#L756)).
- [executeCommand:301–366](../../apps/daemon/src/cloud/CloudControlCommands.cpp#L301)
  dùng khóa từ `withSenderKeys` để kiểm chữ ký/giải mã; không tái kiểm pin ở nhánh
  cache hit. Peer commands có thêm grant check; phát hiện này tập trung vào đường
  relay mailbox. Giữ nguyên workspace giữ nguyên content keys
  ([SessionCredentials.cpp:78–91](../../apps/daemon/src/cloud/SessionCredentials.cpp#L78)).

**Điều kiện/tác động:** Trong cùng process, thiết bị D đã được paired và làm nóng
cache; người dùng sign-out rồi enroll lại cùng workspace trước khi TTL hết. Pin
set mới rỗng nhưng khóa D vẫn được hàm xác thực chấp nhận. D hoặc bên giữ private
key của D có thể ký command phù hợp; relay độc hại có thể chuyển nó với scope
hiện tại. Đây là cửa sổ chấp nhận danh tính đã bị reset, không phải khả năng giả
chữ ký của thiết bị bất kỳ, không phải chứng minh vượt sandbox provider. Những
kiểm tra machine, generation, ciphertext, command kind và idempotency vẫn tồn
tại; command cụ thể cần qua chúng. Với workspace khác, không khẳng định có thể
dùng lại ciphertext/content key cũ.

**Hướng sửa / acceptance cho task sửa:** Xóa cache ở mọi account transition,
fence async lookup bằng epoch và recheck pins trên cache hit; key cache không nên
thay thế authorization hiện hành. Fake fixture: cache D → sign-out → enroll cùng
workspace → lookup D phải bị từ chối cho đến khi duyệt pairing mới. Thêm reply
lookup trễ qua sign-out và revoke; giữ test cache bình thường không gọi mạng thừa.

## DA-03 — Tài liệu trust boundary hứa OS credential store, mã dùng file

**Mức độ: Informational — sai lệch hợp đồng lưu trữ, không chứng minh bypass.**

[daemon-architecture.md:677–679](../daemon-architecture.md#L677) yêu cầu giữ
long-lived device keys trong OS credential store, không plaintext configuration.
[CredentialStore.cpp:22–28](../../modules/remote/src/cloud/CredentialStore.cpp#L22)
chủ động chọn file vì code-signing/keychain prompts;
[:185–205](../../modules/remote/src/cloud/CredentialStore.cpp#L185) serialize giá trị
base64 vào JSON. Base64 không mã hóa. `save()` đặt file 0600 và directory 0700
trên Unix; đó là biện pháp hữu ích nhưng không đáp ứng lời hứa keychain.

Ảnh hưởng: người duy trì/người dùng có thể đánh giá sai bảo vệ at-rest, backup và
tiến trình cùng OS user. Không suy ra người dùng OS khác đọc được file, và không
đánh đồng store này với token IPC đã có descriptor admission chặt hơn. Leader
cần chốt mô hình lưu khóa rồi cập nhật tài liệu/ADR hoặc mở task triển khai store
phù hợp; xác minh riêng ACL/backup cho từng nền tảng. T-1 không đổi storage policy.

## Checks, bằng chứng có sẵn và giới hạn

Đã chạy offline ở worktree baseline:

```sh
node --test apps/daemon/providers/claude-sidecar/test/review-guard.test.mjs apps/daemon/providers/claude-sidecar/test/review-profile.test.mjs
```

Kết quả: **6 passed, 0 failed**. Các fixture chạy sidecar thật với SDK giả trong
thư mục tạm: read review từ chối write, verify chỉ dùng scratch, chặn commit/publish
trong các tình huống test, đổi profile không giữ quyền cũ. Đây không phải test
hồi quy cho DA-01/02 và không chứng minh command sandbox của provider thật.

Kiểm tra báo cáo: `python3 tests/hygiene/test_public_tree.py` **4 passed** (file
báo cáo đã stage nên có trong phạm vi guard); `git diff --cached --check` pass.
Kiểm tra 44 liên kết source: file tồn tại và số dòng anchor nằm trong file.

Đã đọc fixture Qt để đối chiếu nhưng **không chạy**: `AgentMessagingTest`
(`agentConnectionsOnlyReachAgentOperations`, token/deletion cases),
`DaemonProtocolTest` (`approvalHasOneAuthoritativeResolutionAndExpires`),
`cloud-privacy/CloudPrivacyTest` (pin enforcement, cache TTL, enrollment pin reset,
sign-out atomic rewrite và workspace key separation). Các case đã đọc kiểm tra
trạng thái bình thường; không cung cấp bằng chứng đã chặn hai interleaving trên.

Không build native/ctest toàn bộ, không chạy live provider, không probe dịch vụ,
không cài/deploy binary, không đọc private repository và không tạo công cụ khai
thác. Việc mật mã, OS sandbox và hosted authorization hoạt động ngoài mã đã đọc
vẫn chưa được xác minh. Các vùng chưa audit sâu gồm toàn bộ team/schedule role
matrix, mọi filesystem/attachment operation, WebRTC/native transport, mọi adapter
extension và toàn bộ legacy direct transport của `modules/ai`.

Reviewer cần xác minh hai chuỗi trust transition bằng fake-transport regression
trước khi chốt mức độ/release disposition; leader mở task sửa riêng nếu xác nhận.
Không đóng phát hiện chỉ vì sáu sidecar checks pass hay vì một dịch vụ hợp lệ
từ chối request vốn đã đưa key envelope vào body.
