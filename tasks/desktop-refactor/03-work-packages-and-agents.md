# Work packages, multi-agent và thứ tự PR

Đây là phương án thực thi sau khi kế hoạch được duyệt; **không khởi chạy implementation agents trong PR tài liệu**. Mỗi task phải giữ đồng thời functional, API, pixel và performance gates. Không giao một agent “sửa tất cả UI” hoặc cho nhiều agent cùng sửa AgentWorkspace.qml.

**Không task/agent nào được sửa ketjs hoặc ketatlas.** Public Platform chỉ sở hữu ketplus / CM. Backend, daemon, transcript, product UI và mobile nằm trong các repo private tương ứng. Hai dependency bị khóa chỉ được đọc/consume public API/assets đã phát hành; không mở upstream PR, tạo fork/patch hoặc release mới.

## Backlog có dependency / đầu ra kiểm tra được

| Task | Phạm vi / đầu ra | Phụ thuộc | Owner / merge target | Test & exit gate |
| --- | --- | --- | --- | --- |
| R00 | Freeze design + baseline/code audit; inventory 108 state; visual fixture harness plan → thực thi; native before screenshots và performance baseline | PR #16 merged | Integrator + visual QA; Desktop develop | Actual viewer được phép, designer/user xác nhận reference; danh sách native-only gaps; environment record; không auto bless |
| R01 | CM semantic token/geometry/font mapping, public native primitives, reusable settings content/deep-link, generic safe-worktree interface contract; publish CM khi verified | R00 visual freeze; R02 lifecycle interface cho safe remove | Public CM agent; ketplus develop → explicit public release → private gitlink | CM full suite, visual primitive matrix, MPL boundaries; private pin + full private suite do integrator |
| R02 | C01–C05 trong API alignment: versioned contracts, catalog/continuation/canonical item/lifecycle/older-history relay/sign-out semantics; shared fixtures; backend queries/maintenance | R00 baseline | Backend owner; ketviet integration → develop theo repo workflow | OpenAPI + handler tests + DB migration/EXPLAIN/TTL/race tests; fixtures/hash, old/new compatibility; contract freeze trước consumers |
| R03 | Daemon canonical projection + vNext paging/large-item + scoped operation fencing + bounded async migration/outbox; no eager whole-history attach | R02 frozen fixtures/schema | Daemon agent; Desktop develop | Protocol/lifecycle/client tests; giant event within encoded budget; lossless page/live; output absent from relay; crash/retry |
| R04 | Private application navigation/scope store; presentation state boundaries; preserve drafts/selection; app restore overlay render-first; extract cohesive QML composition | R00, R01 exported UI contract | Shell agent; Desktop develop | Scope-switch late-response tests, startup smoke, close/hide không stop turn, all shell integration tests; screenshot skeleton |
| R05 | Transcript window/model: stable item IDs, recent 3 turn + continuation, merge/revision, stable anchor/Latest, byte-bounded LRU | R02/R03; presenter interface R04 frozen | Transcript-model agent; Desktop develop | 1k/10k + giant turn, stale epoch, no loss/duplication/reset, bounded windows, anchor tests |
| R06 | Transcript renderer: incremental Markdown blocks/tail, code/table/Mermaid/media lazy/cache, command single row, selection/copy | R05 model roles frozen; R01 tokens | Transcript-render agent; Desktop develop | Fidelity fixtures + lazy load; Mermaid pinch/SVG; parse/layout counters; full-copy semantics; malformed/untrusted content |
| R07 | Workspace/composer/question card/status panels mapped đủ 41 state; shared compact Atlas chat presentation | R04/R05/R06 | Workspace UI agent; Desktop develop | Send/steer/stop/answer/expiry/missing lifecycle, draft/attachment; pixel pack workspace; no monolith growth |
| R08 | Archive/delete/restore durable resource service + IPC consumer + worktree manager + private Archived SettingsPage | R02/R03, R01 safe-remove API/pin, R04 state | Resource-lifecycle agent; Desktop develop; backend contract already in R02 | TOCTOU/crash/restart/purge/tombstone tests; dirty/main/active/unsaved guards; stay Settings; pixel archive/delete/worktree packs |
| R09 | Connections presenters/dialogs: auth errors/sign-out IPC, Add QR, approve/cancel/revoke, responsive constrained layout | R00/R01, A07–A11 frozen; R04 shell | Connections agent; Desktop develop | RemoteControlView tests, fake relay lifecycle, release-bypass absence, all 20 state screenshots |
| R10 | Atlas library/builder/preview: thumbnail cache, private integration adapter/IDs, fixed overlay, last-valid, missing CLI/bundle | R01/R04/R07 compact-chat interface; audit supported API của KetAtlas đang pin | Atlas agent; chỉ ketplus-desktop develop, không sửa ketatlas | Atlas + workspace tests, system WebView/native stacking/focus/pan/pinch, all 17 Atlas states; bounded process/cache metrics; thiếu supported API là blocker, không upstream extension |
| R11 | Native editor/preview/diff/terminal surface fidelity without engine rewrite | R01 public release/pin, R04 shell | CM surface agent; ketplus public first, then private composition | CM full suite + private integration; 6 state screenshots, dirty documents/IME/terminal lifetime regression |
| R12 | Settings appearance host + private Archived composition + Spotlight routing/keyboard/archived filtering | R01/R04/R08; R11 appearance integration | Shell/settings agent; Desktop develop; public changes upstream first | Settings/Spotlight tests, appearance propagation, deep-link/restore stay Settings, pixel settings/search packs |
| R13 | iOS Control vNext parity: item key fix, actor-backed bounded cache, continuation/epoch, catalog/tombstones/old-history relay; rich transcript parity | R02 fixtures → R03 integration; R06 presentation spec | Mobile agent; ketplus-app develop | swift test + simulator + contract check; same fixtures; main-thread/cache/Latest; physical iOS profiling later, no upload yet |
| R14 | Full native pixel + performance + local integration, then staging/E2E readiness and release handoff | R00–R13 accepted | Integrator/QA; per-repo develop PRs | All state/node/edge checks, zero unexplained visual diff, perf budgets, mixed versions/reconnect/restore/revoke/TTL; evidence ledger |

R02 có thể chia PR contract/fixtures trước và server handlers/query/migrations sau; consumers chỉ implement against **frozen schema + fixtures**, không gọi server route chưa triển khai. R03/R08 cùng sở hữu SessionHost/LocalJournal nhưng **không chạy đồng thời trên các file đó**: R03 merge xong, R08 rebase mới làm resource mutations. R07/R10 chia sẻ chat qua interface, không copy composer riêng.

## Các interface cần freeze (không phải class đã tồn tại)

| Contract | Owner duy nhất khi freeze | Consumer / rule |
| --- | --- | --- |
| Canonical types + camelCase/snake_case mappings + fixture version | R02 | Daemon/mobile/Desktop; đề nghị đổi schema phải quay về owner |
| UI semantic tokens/metrics + CM settings host + safe Git primitives | R01 | Private shell/AI/Atlas/Connections; chỉ exported KetPlusCM::* APIs |
| SelectionContext + scoped draft/operation projection | R04 | R05/R07/R08/R09/R10/R12; không giữ selection singleton riêng mỗi view |
| Transcript item/window roles + anchor + continuation API | R05 | R06/R07; event != item; no row-index persistence |
| Compact workspace/composer API | R07 | R10; lifecycle do session store sở hữu |
| Resource lifecycle/status + catalog invalidation | R02/R08 theo server/local boundary | R04/R12/R13; không định nghĩa lại “deleted” ở UI |
| Atlas bundle/flow/preview consumer adapter | R10, chỉ private Desktop | R04/R07/R12; dùng supported public API, không sửa viewer; preferred context không thành quyền filesystem |

## Lịch chạy song song: tối đa 4 agent đang hoạt động

Một integrator + tối đa ba worker. Một task độc lập có thể tiếp tục ở worktree riêng trong khi integrator review task khác; không giả định có 10 slot cùng lúc.

| Wave | Worker A | Worker B | Worker C | Integrator / barrier |
| --- | --- | --- | --- | --- |
| 0 | R01 audit CM/token/interface, chưa port pixel trước freeze | R02 API/schema/fixtures | R00 inventory & capture/harness khi có quyền | Khóa design/base SHA, phân file, chốt C01–C05; freeze contract + reference |
| 1 | R01 public implementation/release gate | R03 daemon sau contract freeze | R04 private shell/state trên exported interface đã khóa | Review public first; một người advance gitlink; không promote unverified API |
| 2 | R05 model/paging | R08 lifecycle sau R03 + CM pin | R09 Connections | Model role freeze; R08 không chạm lại R03 chưa merge; integration smoke |
| 3 | R06 renderer | R13 iOS schema/cache/parity (rich phần sau chờ R06 spec) | R11 public surfaces | Native pixel review từng slice, propagate tested CM release pin |
| 4 | R07 workspace UI | R12 settings/search sau R08 | R10 Atlas library/index/bridge trước, compact-chat integration chờ R07 | Tách file shell composition rõ; chỉ integrator sửa common root CMake/registration |
| 5 | R10 hoàn tất compact chat/pixel | R13 rich parity + device profiling | R14 QA/benchmark fixes trong phạm vi được giao | End-to-end all repos, visual sign-off, consolidated readiness |

Critical path: **R00 → R01/R02 → R03/R04 → R05 → R06 → R07 → R10 → R14**. Resource lifecycle path R02/R03/R01 → R08 → R12/R13 → R14 cũng là blocker. Nếu một barrier chậm, worker chỉ làm fixture/test độc lập; không tự invent API để “unblock”. Thời lượng chỉ estimate sau baseline R00, không cam kết số ngày khi chưa benchmark.

## Worktree / branch discipline

- Trước mỗi task: đọc AGENTS.md của repo sở hữu, fetch, kiểm tra worktree/dirty state, ghi exact base. Không reuse main checkout hoặc worktree của agent khác.
- Desktop/public CM lấy origin/develop mới nhất đã qua dependency gate. Backend lấy origin/integration chứa baseline Control v2, base/target PR theo workflow backend; mobile runtime lấy origin/develop. Nếu policy upstream đổi, integrator cập nhật baseline trước spawn.
- Mỗi worker một worktree/branch/task/repo; prefix đề xuất refactor/rNN-short-name, thư mục sibling kết thúc -rNN. Có thể có nhiều worktree Desktop nhưng tuyệt đối không chung build directory.
- Compiler object cache có thể dùng cùng sccache store theo config hiện hữu; build/DerivedData/DB/socket/token/log/screenshot dirs riêng từng worktree. Không symlink build tree giữa branches, không dùng daemon/user journal đang chạy làm fixture.
- Không commit absolute machine paths, user transcripts, credentials, caches/generated builds. Shared fixtures chỉ synthetic; screenshots dùng sanitized data, không dùng conversation thật.
- Một PR gồm một task hoặc slice cohesive. Public CM code release trước, private chỉ nâng production cm-v* gitlink; không pin CM develop để né release. ketjs/ketatlas không được sửa hoặc release trong phase này; không thêm dependency vào upstream extension chưa tồn tại. Không tự nâng pin để thay reference đã duyệt.
- Common files có **một writer**: root CMakeLists/tests registration/gitlink/atlas.progress.json/coverage mapping do integrator tích hợp; workers nộp patch/spec cho các file này. Không chạy blanket formatter hoặc git add toàn worktree bẩn.
- Rebase trước PR vào đúng base đã merge dependencies; chạy lại affected matrix. Không force-push main/develop. Không publish private resource/auth/transcript logic sang public CM.

## Prompt giao worker (template)

~~~text
Bạn thực hiện duy nhất TASK_ID theo tasks/desktop-refactor/ trong repository sở hữu.
Đọc AGENTS.md và task/API/pixel gates trước khi sửa. Fetch base đúng lane, tạo worktree riêng.
Không vượt scope repo/file đã phân công; không sửa vendor/ketplus-cm.
Không sửa ketjs hoặc ketatlas (source/assets/tests/build/release); không fork/vendored patch hay dùng private internals để lách giới hạn.
Chỉ consume public API/assets hiện hữu; adapter nằm ở owning CM/private repo. Không đáp ứng được thì báo blocker, không giảm pixel/performance gate.
Base SHA: <integrator cung cấp sau fetch>. Allowed files/modules: <danh sách cụ thể>.
Dependencies đã merge/freeze: <SHA contract + fixtures + public pin + model roles>.
Nếu API chưa có hoặc design mâu thuẫn, báo evidence và yêu cầu owner giải quyết; không invent endpoint/state.
Reuse engines; không hard-code mock data vào runtime. Bảo toàn scoped IDs, ack semantics, stale-response fencing.
Pixel-perfect là blocking gate theo 04-pixel-performance-gates.md; không tự sửa baseline/tolerance.
Thêm focused tests và fixtures. Benchmark Release nếu chạm performance; không gọi provider thật/quota.
Không xóa dữ liệu/worktree thật, thay daemon đang chạy, deploy hay upload ứng dụng trong task này.
Handoff: commit/PR, files changed, API/schema diff, tests với môi trường/revision,
reference/native/diff screenshots nếu có quyền, metrics, known gaps và dependency tiếp theo.
Chỉ đánh dấu pass cho việc đã chạy. Không merge PR của mình thay cho independent review gate.
~~~

Integrator phải điền allowed files trước khi chạy, không gửi template rỗng. Ví dụ R05 chỉ AgentMessageModel/AgentSessionHistory/new transcript-store + focused tests; R06 chỉ renderer/formatter/media/leaf QML + tests; R07 mới chỉnh AgentWorkspace.qml composition. Nếu cần thay interface chung, worker gửi proposal, integrator schedule serial patch.

## Review, rollout và rollback

1. Worker tự review diff; một reviewer không phải tác giả kiểm tra data races/security/identity/pixel evidence. Review chạy song song với worker khác chỉ khi có slot. Không coi self-review đơn độc là đủ cho destructive lifecycle/schema.
2. Mỗi screen nộp native/reference/diff + functional test/contract hash; reviewer đối chiếu state inventory. Missing image/permission = gate chưa đạt, không tick verified.
3. Merge nhỏ theo dependency; integrator chạy cross-module suite sau shell/header/gitlink changes. Tích hợp qua typed interfaces; không flag mặc định bật incomplete state.
4. Dùng internal rollout flags theo feature nếu cần (redesigned shell, transcript vNext, lifecycle), không tạo fallback làm mất tombstones hoặc eager full replay. Old read-compatible UI chỉ là rollback UX, không downgrading encrypted schema.
5. Canary local bằng synthetic sessions trước; staging E2E cần URL/config/deployment authority riêng. Xác minh auth/revoke/TTL/key rotation/cursor/mixed versions trước TestFlight readiness.
6. Sau code/tests/visual/perf pass mới chuẩn bị desktop build + paired iOS acceptance. Build/archive/upload iOS là **bước cuối riêng**, cần Apple team/signing/App Store Connect choices; không ghi “TestFlight ready” chỉ vì simulator xanh.

## Progress / handoff record

Giữ atlas.progress.json: Planned → In progress → In review → Implemented (code merged) → Verified (mọi recorded check có passed test evidence/revision/environment, không blocker). Planning checks đều false; giữ evidence/check IDs từ PR #16. Mọi state trong coverage phải có acceptance evidence mapping, không lấy 1 screenshot đại diện để claim cả screen.

Handoff mỗi PR có: task IDs; base/head/merge SHA; upstream CM/contract/KetAtlas pins; owning files; fixture hashes; exact test commands + outcomes; reference environment; screenshot/benchmark artifacts; migration/rollback; còn gì unverified. Private implementation >1,000 lines phải có lý do rõ trong change description; ưu tiên tách cohesive component trước khi vượt 500 dòng, không tách theo số dòng tùy tiện.
