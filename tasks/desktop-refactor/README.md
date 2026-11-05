# Kế hoạch refactor Desktop theo KétAtlas

Ngày lập: 2026-09-12. Phạm vi PR này: **kế hoạch và tracking**, không phải native implementation, release hay deployment.

## Kết quả phải đạt

Áp toàn bộ thiết kế PR #16 vào KetPlus native, đúng flow, đúng dữ liệu và **pixel-perfect** tại môi trường tham chiếu đã khóa. Không chỉ đổi màu/spacing; không đưa HTML mock hoặc URL demo-state vào runtime sản phẩm.

- 10 màn hình, 10 flow, 115 node / 152 edge, 108 URL màn hình/state duy nhất.
- Giữ Qt Widgets + Scintilla cho editor, Qt Quick cho transcript, system WebView cho Atlas. Không viết lại cả IDE bằng WebView.
- Desktop đi qua daemon IPC; mobile đi qua Control API. KetSuite là control plane headless, không thêm UI KetSuite và không đưa Zitadel vào phase này.
- Codex-only khi giao sản phẩm; canonical transcript phải mở rộng được bằng adapter, không giả vờ đã hỗ trợ Claude/Cursor/KétRouter.
- Giữ hybrid: daemon SQLite là full-history authority; cloud là encrypted hot replica tối đa 24 giờ; mobile có account-scoped encrypted cache. Command chỉ có lệnh/trạng thái, không đưa output lên cloud/mobile.
- Archive/Delete có acknowledgment và chống resurrection khi reconnect; restore **chỉ từ Settings → Archived**, giữ nguyên ownership và không chạy lại turn.
- Không đánh đổi tính đúng, accessibility hoặc performance để lấy ảnh đẹp. Pixel-perfect, functional và performance là ba gate độc lập, đều bắt buộc.

## Ràng buộc repository đã chốt

**Không sửa ketjs hoặc ketatlas trong phase này.** Hai repo là dependency read-only: chỉ đọc tài liệu/source và sử dụng public API/assets đã phát hành. Không tạo PR, commit, patch/fork, sửa generated/vendor assets hoặc phát hành phiên bản mới ở hai repo đó để phục vụ refactor.

- Public code được phép triển khai nằm ở **ketplus / KetPlus CM** theo ownership và quy trình release/pin hiện có. Không sửa vendor/ketplus-cm tại private Desktop.
- Private code nằm ở ketplus-desktop (bao gồm daemon), ketviet và ketplus-app. Backend chỉ dùng KetJS APIs hiện hữu; thiếu framework API không authorize sửa ketjs.
- Native token/geometry mapping nằm trong public CM; product composition và Atlas integration adapter nằm trong private Desktop. Không đổi canonical DS tokens/asset hashes hoặc vá KetAtlas viewer internals để đạt pixel-perfect.
- Nếu public API hiện hữu không đáp ứng: thử private adapter/composition qua interface được hỗ trợ; nếu vẫn không giữ được flow/fidelity/performance thì ghi blocker kèm evidence và xin quyết định phạm vi. Không tự giảm tiêu chí nghiệm thu, fork engine hoặc thêm upstream work ngoài phạm vi.

Bundle thiết kế .ketatlas trong private Desktop là artifact của sản phẩm, không phải repository ketatlas. Tracking/kế hoạch ở repo private vẫn được cập nhật; canonical dependency source/assets không thay đổi.

## Tài liệu thực thi

| Tài liệu | Nội dung |
| --- | --- |
| [01 — Matrix](01-screen-feature-matrix.md) | Màn hình, flow, tính năng, owner, gap, acceptance |
| [02 — API](02-api-alignment.md) | API đang có, mismatch, contract cần bổ sung, migration |
| [03 — Work packages](03-work-packages-and-agents.md) | R00–R14, dependencies, worktree/agent lanes, PR order |
| [04 — Pixel & performance](04-pixel-performance-gates.md) | Reference khóa, screenshot gates, benchmark và DoD |
| [Coverage](coverage.json) | Inventory có thể kiểm tra tự động: mỗi state → node → screen → task |

Nguồn thiết kế: [DESIGN](../../ketplus.ketatlas/DESIGN.md), [FLOW-CONTRACT](../../ketplus.ketatlas/FLOW-CONTRACT.md), [review resolution](../../ketplus.ketatlas/REVIEW-RESOLUTION.md). Ownership: [repository boundaries](../../docs/repository-boundaries.md), [architecture](../../docs/architecture.md), [ADR 0004](../../docs/decisions/0004-canonical-transcript-hybrid-retention.md).

## Baseline đã kiểm tra

Các ref dưới đây đã được fetch/đối chiếu ngày lập kế hoạch; đây là code evidence, không phải deployed evidence. Mỗi agent phải fetch lại trước khi nhận việc, ghi SHA vào PR và đánh giá drift.

| Repo / surface | Ref | SHA |
| --- | --- | --- |
| Desktop runtime | origin/develop | 4b1412112b2f45a1e59689658f3b54f08439b133 |
| Thiết kế đã sửa P1/P2, PR #16 | origin/design/desktop-ketatlas | 5534a4cb0c698f3c4a2c0d2ec3ad50384680f0fa |
| PR #16 đã merge; base của PR kế hoạch | origin/develop sau squash merge | 7efc058d99b37f269fdebd5b69044fff32b84195 |
| Backend KetPlus hiện tại | ketviet origin/integration | 65f1e35fffffd299cfb4f93f4b24982e2b1bf807 |
| Backend develop, **chưa chứa baseline trên** | ketviet origin/develop | fef34104548917b7da0126faaf358382fa36fd8b |
| iOS runtime / client | ketplus-app origin/develop | fdd84d4fcdf94fd5e5427ca2ee8a99530a25f70f |
| Mobile main, không thay thế runtime develop | ketplus-app origin/main | e7deb9795f7e626cb1268d17e91a8d9d86f4d66e |
| Public CM được Desktop pin | cm-v0.1.5 | 9fb86c8ccf64bb75e1e5cdf31314cedf46d24439 |
| Public CM mới nhất, **chưa tự động được pin** | ketplus origin/develop | 7ac9d758f1b095b4f85362d40fe4b6e55c261680 |
| Két Design System của thiết kế | @ketvietlab/design-system 0.1.7 | 6f923d1e13958ce740e50bccab45efc56ccd2f84 |

Không dùng nhánh backend develop cũ để bắt đầu rồi vô tình mất Control API v2. Backend PR xếp trên integration; promote vào develop theo workflow của ketviet, không trộn cả integration vào PR Desktop. Không dùng mobile main để ghi đè fixes của develop. Không sửa submodule tại chỗ.

## Matrix readiness tổng hợp

Quy ước: 🟢 có source/contract tại baseline; 🟡 có một phần hoặc cần tích hợp/kiểm chứng; 🔴 thiếu và chặn acceptance. Không có tỷ lệ phần trăm hoàn thành suy đoán.

| Gate | Hiện tại | Việc còn lại |
| --- | --- | --- |
| Thiết kế và graph | 🟢 Có 108 state; 131 DOM/model tests của PR #16 | R00: duyệt actual viewer/focus/viewport và đóng băng reference |
| Native áp đúng design | 🔴 Chưa có bằng chứng pixel | R01/R04/R07–R12 + visual gate từng màn hình |
| API auth/pairing/commands | 🟡 Có v2 handlers, daemon và iOS client | Sign-out IPC, errors, contracts/compatibility và end-to-end |
| Transcript bounded, lossless | 🔴 Page có truncated nhưng thiếu continuation trong turn | R02/R03/R05/R06/R13 |
| Archive/Delete/Restore | 🔴 Mock only | R02/R08; tombstones, guards, purge acknowledgment và resync |
| Public CM fidelity | 🟡 Có primitives/pin, chưa port approved geometry | Public release + private pin sau tests |
| Native performance | 🔴 Chưa benchmark bộ fixture mục tiêu | R14: Release, raw metrics, native screenshots |
| Staging / TestFlight | 🔴 Không chứng minh được bằng PR design | Deployment/auth/key/retention E2E; iOS signing/build/upload là bước cuối riêng |

## Quyết định và điểm cần đóng ở R00/R02

1. Pixel-perfect là yêu cầu mới có ưu tiên cao hơn câu “native controls may differ slightly” trong docs/design-system.md. R01 phải cập nhật tài liệu đó cùng implementation; không dùng câu cũ để miễn visual acceptance.
2. “3 phiên cuối” trong transcript được triển khai theo quyết định hiện hữu là **3 turn mới nhất trong conversation**, không phải 3 conversation. Phải có item/byte budgets và continuation trong một turn lớn.
3. Thêm archive/delete/restore vào protocol là **đề xuất**, chưa phải API được duyệt. Chốt schema, capability, ownership, completion semantics và migration trước khi UI gọi.
4. Native design mock chưa được kiểm tra trong actual viewer do quyền browser đã bị từ chối. R00 cần quyền hoặc người dùng kiểm tra thủ công; không dùng công cụ khác để vượt quyền. Không đánh dấu Verified khi thiếu bằng chứng này.
5. Không tự đổi DS lên bản mới, tự thay font/brand, nới pixel threshold hoặc regenerate goldens để che mismatch.
6. Không có team/task nào được giao implementation trong ketjs hoặc ketatlas; các đề xuất upstream extension trước đây bị loại khỏi kế hoạch. Giữ public CM làm lane public duy nhất có thay đổi source.

## Quy trình PR của kế hoạch

PR #16 đã squash-merge thành 7efc058; remote design branch cũ đã được xóa. Worktree kế hoạch đã chuyển base sang commit này, không mang lại chuỗi commit design cũ. Push **chỉ commit kế hoạch/tracking mới** lên design/desktop-ketatlas rồi mở PR mới vào develop. Nếu branch được người khác tái tạo trước push, kiểm tra lại head; không tự force-push hoặc ghi đè công việc khác.

PR kế hoạch không authorize sửa runtime, xóa worktree thật, thay daemon đang chạy, phát hành CM, staging deploy hay upload TestFlight. Các implementation PR sau tuân theo package/gate tương ứng.

## Kiểm tra tài liệu

~~~sh
node tasks/desktop-refactor/check-plan.cjs
git diff --check
~~~

Checker chỉ xác minh inventory, links, DS hashes và tracking có đủ scope; không render pixel hoặc chứng minh API/native hoạt động. Theo docs/testing.md, PR kế hoạch không cần chạy lại native build. Các harness/fixture/benchmark được yêu cầu trong R00–R14 là deliverable tương lai, không phải file đã tồn tại chỉ vì được nêu ở đây.

Verification của change kế hoạch: Node 24.14.1 inventory/syntax PASS; KetAtlas validate + strict audit PASS (10/10/115/152, 47 local files, zero warnings/remote URLs); 131 DOM/model tests của design chạy lại PASS; git diff --check PASS. Chưa có native build/pixel/performance hoặc real-backend test trong change này.
