# Ma trận màn hình và tính năng

Phạm vi là **refactor native theo design**, không phải viết lại các engine đang có. 🟢 = có nền tảng source; 🟡 = cần port/tích hợp/verify; 🔴 = thiếu contract/implementation. Mọi screen còn thiếu pixel evidence. ID task tra ở [work packages](03-work-packages-and-agents.md); A01–A14 là contract ở [API alignment](02-api-alignment.md).

Ownership constraint: ketjs và ketatlas là read-only. Lane public trong matrix chỉ được sửa ketplus / CM; Atlas bridge là consumer adapter trong private Desktop dùng public API hiện hữu, không phải task mở rộng viewer upstream. Thiếu API/fidelity không thể giải quyết trong phạm vi phải ghi blocker.

## Màn hình → owner → acceptance

Số state là URL duy nhất tính cả entry screen. Một state có thể xuất hiện trong nhiều node. [coverage.json](coverage.json) liệt kê chính xác toàn bộ URL/node, tránh chỉ kiểm tra màn hình happy path.

| Screen ID | State / node | Nền native hiện có | Refactor / API dependency | Owner, task | Điều kiện nghiệm thu đặc thù |
| --- | --- | --- | --- | --- | --- |
| workspace | 41 / 43 | 🟢 AgentWorkspaceView, AgentSessionManager, QML ListView, daemon attach | 🔴 Paging lossless; 🟡 unified scope/lifecycle/composer, restore overlay; A01–A06/A12 | Private AI + desktop + daemon; R03–R08 | Nhiều conversation/worktree, draft/attachment/anchor không lẫn; send/answer/stop đúng ack; không mất item; đúng 41 state |
| editor | 3 / 4 | 🟢 Scintilla, CM documents/preview/terminal | 🟡 tab/toolbars/geometry/token; local APIs A13 | Public CM + private composition; R01/R04/R11 | Edit/save/undo/selection/IME/Markdown preview thật; context terminal dùng đúng pane, không thêm engine giả |
| changes | 3 / 3 | 🟢 CM Git status/diff | 🟡 Changes/Staged/read-only UI; A13 | Public CM + private composition; R01/R11 | Clean/staged/unstaged đúng Git, gutter +/-; không tự stage/commit từ nút mock |
| worktrees | 11 / 14 | 🟢 discovery + AgentWorktreeCreator | 🔴 lifecycle metadata/delete; A02/A12/A13 | Private shell/daemon + public safe worktree operation; R04/R08 | Distinct IDs, create pending/error; dirty/main/active guards; archive vẫn cho monitor/Stop |
| atlas-library | 2 / 3 | 🟢 AtlasIndex, AtlasTab picker | 🟡 actual thumbnail/list/grid/filter, bounded preview; A14 | Private Atlas; R10 | Đúng bundle/worktree, empty/recovery; không mở WebView/process cho mọi card |
| atlas-builder | 13 / 13 | 🟢 AtlasBuilderTab + compact AgentWorkspaceView | 🟡 overlay/map geometry, stable bundle/flow bridge; A01/A14 | Private desktop + AI + Atlas; R07/R10 | Chat ≤400 px/inset 18 px; hide không resize map; last-valid preview; @Flow không hứa edit isolation |
| atlas-preview | 2 / 2 | 🟢 system WebView + CLI process | 🟡 identity/CLI missing/recovery + native stacking | Private Atlas; R10 | Bundle đúng; giữ map state; install chỉ explicit; preview không phụ thuộc turn completed |
| connections | 20 / 20 | 🟢 RemoteControlView + CloudControlClient | 🟡 redesign + errors, 🔴 sign-out IPC; A07–A11 | Private remote UI/daemon; R09 | Signed-out chỉ Đăng nhập; Add → QR có Close; confirm/cancel/revoke có ack; không lộ secret; Unplug icon |
| settings | 10 / 10 | 🟢 CM SettingsDialog/SettingsPage + appearance | 🔴 embedded composition/deep-link Archived, lifecycle; A12/A13 | Public settings shell + private Archived page; R01/R08/R12 | Restore ở Settings và ở lại đó; giữ owner; missing folder bị chặn; appearance áp cả private + CM |
| search | 3 / 3 | 🟢 SpotlightDialog + RepositoryRouter | 🟡 visual/keyboard/scope filtering; A02/A13/A14 | Private routing + public search primitives; R04/R12 | @repo, no-match, keyboard focus; đúng worktree; loại archived khỏi mặc định, không mở sai root |
| **Tổng** | **108 / 115** | Các engine có thể tái sử dụng | **0 screen có bằng chứng native pixel-perfect tại baseline** | R00–R14 | Mọi flow/state đều có functional + visual evidence |

## Ma trận tính năng xuyên màn hình

| ID | Tính năng / semantics phải giữ | Gap tại baseline | Contract / task | Fixture và test bắt buộc |
| --- | --- | --- | --- | --- |
| F01 | Shell: sidebar 216 px, chat max 760 px, common typography, focus, selected khác running | 🟡 Design/native có khác biệt màu/status/layout | A13; R01/R04 | Light/dark; long name; window resize; status không dựa riêng màu |
| F02 | Repo → worktree → nhiều conversation | 🟡 Có registry, cloud là session list và label optional | A02; R02/R04 | Hai repo cùng title/branch, hai conversation/turn chạy song song, draft chưa có daemon session |
| F03 | Draft/attachment/reading position theo scope | 🟡 Phải audit và unify persistence, không dùng URL demo snapshot | A01/A02; R04/R07 | Switch/reopen/restart; ack muộn; upload lỗi không mất input |
| F04 | Send/steer/stop/answer tách queued/accepted/working/final | 🟡 Current APIs có nhưng full design parity chưa verify | A03/A04; R03/R07 | Stop-race/completion, answer expired/answered elsewhere, rejected command, retry cùng key |
| F05 | Daemon connection, folder availability, history load, turn activity là 4 trục riêng | 🟡 Không map reconnect thành running/completed | A01/A03; R04/R07 | Mac offline, folder missing trong khi turn còn chạy; Stop vẫn được phép nếu transport còn khả dụng |
| F06 | Initial 3 turn + load older + within-turn continuation | 🔴 Daemon/cloud chỉ có truncated, client dễ bỏ sót | A05; R02/R03/R05/R13 | >500 event/turn; multi-MiB item; page/live overlap; opaque token expired |
| F07 | Item-level canonical projection, command lifecycle | 🔴 iOS assistant index theo turn; normalization chưa đủ adapter-neutral | A06; R02/R03/R05/R13 | ≥3 assistant items xen tool trong một turn; delta/completed/duplicate/stale/reorder |
| F08 | Reading anchor + Latest thật | 🟡 UI virtualization có, bounded model chưa đủ | A05; R05/R07/R13 | Đọc phía trên khi stream; prepend/evict; width/font/media reflow; Latest lúc newest bị evict |
| F09 | Bounded RAM/parse/cache, restore ưu tiên selected | 🔴 Eager attach + growing body/layout/data | A05/A06; R03/R05/R06 | 20 conversation, 1k/10k item và giant turn; app launch overlay có frame, không main-thread stall |
| F10 | Markdown/code/table/list/link/copy/diff/Mermaid/media | 🟡 Desktop có nền; mobile rich parity thiếu | A06; R06/R13 | Nested markdown, unfinished fences, full copy, large table, lazy media, Mermaid SVG scale + touchpad pinch |
| F11 | Command chỉ 1 row Run/Ran/Failed/Interrupted; cloud/mobile không output | 🟢 Daemon CloudEventProjection đã strip output | A06/A11; R03/R06/R13 | Output marker không tồn tại trước encrypt/upload/retry-cache; giữ nguyên full command text, không cắt 100 ký tự |
| F12 | Archive worktree/conversation không dừng turn | 🟢 Local desktop lifecycle + Settings Stop; backend/mobile revision-fenced session archive | A12; R02/R08 | Local restart + cloud CAS/heartbeat no-resurrection; worktree cloud sync remains out of scope |
| F13 | Restore Settings → Archived, không rerun | 🟢 Private Archived page in public settings host; restore stays in Settings | A12/A13; R01/R08/R12 | Original path preserved; missing worktree is not rebound to another path |
| F14 | Delete conversation vĩnh viễn trong KetPlus, không xóa provider history | 🔴 Tombstone/purge protocol thiếu | A12; R02/R08/R13 | Typed DELETE; active blocked; outbox replay/crash; offline mobile lâu hơn cloud TTL |
| F15 | Delete clean linked worktree, giữ branch, archive history | 🔴 Safe remove coordination chưa có | A12/A13; R01/R08 | Main/dirty/untracked/locked/active/unsaved buffer blocked; failure không mất data; no force |
| F16 | Sign-in bằng KetSuite username/password, auth vs service errors | 🟢 v2 auth, iOS error fix; 🟡 native parity | A07; R09/R13 | 401 vs timeout/TLS/5xx/429; refresh race; logout failure; Debug-only bypass không vào Release |
| F17 | Add QR → mobile claim → Mac approve, cancel/revoke | 🟢 Contract và implementation; 🟡 20-state parity | A08–A10; R09 | Close trước/sau claim, delayed approve, revoked device, expiry, failed cancellation; không show connected sớm |
| F18 | Stable Atlas bundle + flow context + conversation | 🟡 Native primitives có, composite bridge thiếu | A14; R10 | Rename/missing bundle, change visible flow không đổi tag; draft giữ khi collapse |
| F19 | Atlas thumbnail/preview resource lifecycle | 🟡 Lazy WebView có; mock thumbnails chưa production | A14; R10 | 100 bundles; burst invalid writes; retain last-valid; idle teardown; bounded processes |
| F20 | Appearance, typography, shortcuts, a11y | 🟡 Shared settings có, approved screen chưa áp | A13; R01/R12 | Live preview/save/cancel/restart; Inter VN glyphs; keyboard/VoiceOver/reduced motion |
| F21 | Editor/Git/terminal không regression | 🟢 CM foundation, chưa redesign | A13; R11 | Dirty document close/save/cancel, terminal survives hide, indexed/staged state đúng, all CM tests |
| F22 | Catalog/history convergence desktop/mobile | 🔴 Paging/archive extensions chưa có | A02/A05/A12; R02/R03/R13 | Old/new clients, generation change, revoke, tombstone-before-history, no resurrect |

## Flow regression pack

| Flow | Bắt buộc kiểm tra ngoài ảnh tĩnh |
| --- | --- |
| agent-journey | Submit → ack → turn; multi-question/private input; stale answer; stop/steer failure; reconnect/missing folder; scope switch giữa request |
| code-review | Editor → preview → Changes/Staged → terminal → refresh Git; không phát sinh mutation ngầm |
| worktree-journey | Validation → pending → success/error; repository identity thật; missing/recovered không retarget history |
| atlas-journey | New distinct bundle, generating + valid-preview cùng lúc, invalid candidate giữ old preview; flow tag independent; missing CLI |
| device-journey | 20 state gồm sign-in/sign-out, offline, expired, cancel-error và revoke-error; callbacks bị fence theo epoch |
| appearance-journey | Shared snapshot áp lên native shell/QML/Scintilla/terminal/preview; Settings cancel không lưu |
| spotlight-journey | Keyboard open/search/filter/select/escape; focus trả về vị trí cũ; archived mặc định bị loại |
| history-journey | Earlier/partial/error/offline boundary, stream while reading, Latest, rich-load-error; tuyệt đối không silently truncate |
| archive-journey | Parent-child archive độc lập, restore ở Settings, running archive vẫn kiểm soát được, resync không double-restore |
| delete-journey | Dirty/main/active block; typed confirm; TOCTOU/crash/purge-failure; archived history vẫn ở original scope sau worktree delete |

## Những surface chưa được mock đầy đủ

Design inventory không thay thế regression inventory của sản phẩm. R00 lập thêm native fixtures cho app-level restore overlay, About/notices, dirty-document prompt, dialogs hệ thống, unknown provider event, read-error/permission denied, disk-full/migration failure. Không tự thiết kế lại chúng trong implementation PR: giữ hành vi hiện có hoặc xin bổ sung design có version. Không đưa provider/billing/organization UI tương lai vào phạm vi 108 state.
