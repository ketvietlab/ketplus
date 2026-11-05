# Pixel-perfect và performance: blocking acceptance

Yêu cầu của người dùng là **áp design vào KétPlus phải pixel-perfect**, không phải “đúng tinh thần”. Ngưỡng đã duyệt là tỷ lệ pixel khác reference tối đa **5% cho từng state/viewport/theme**. Thiếu control, sai flow, text bị cắt, hit target hoặc safe-area vẫn fail dù tỷ lệ tổng dưới 5%; không được che vùng hoặc nới tolerance hình học để qua gate. Đây là tiêu chí chặn merge/acceptance cho implementation màn hình. Các con số performance dưới đây là **target đề xuất để chốt tại R00**, chưa phải số đo hiện tại. Không nới gate sau khi fail mà thiếu quyết định có bằng chứng.

## G0 — Khóa reference trước khi port

1. PR #16 merge trước; ghi merge SHA và design content SHA 5534a4c vào reference manifest. Designer/người dùng xem actual screen trong KetAtlas **Try this screen**, không chỉ xem graph hoặc ảnh chat. 131 DOM/model tests chỉ chứng minh một phần hành vi mock.
2. Pin Két Design System 0.1.7 / revision 6f923d1e13958ce740e50bccab45efc56ccd2f84 và ba asset hashes trong design-system.lock.json. Pin Inter assets/weight/metrics, Lucide Static 1.40.0, KétPlus v1 brand originals. Không thay wordmark bằng text/icon tự vẽ; không re-encode brand tùy tiện.
3. Khóa OS build, Qt patch version, font resource SHA, locale, color profile, screenshot pipeline, DPR, Metal renderer, animation clock, fixture SHA. Web reference khóa browser/WebView version cùng máy/OS khi có thể. Nếu backend fixture thay copy/timestamp/title, khóa lại fixture chứ không so ảnh dữ liệu khác.
4. Đo **client content rect 1440 × 900 logical px**, không lấy kích thước cửa sổ gồm titlebar/shadow. Reference web zoom 100%, product typography scale 115%. Native logical coordinate tương ứng CSS px; không scale toàn screenshot cho vừa.
5. Primary reference macOS arm64, DPR 2 → 2880 × 1800 physical px cho content rect. DPR 1 là pack riêng; không resize goldens DPR 2 để giả kiểm tra DPR 1. Các nền tảng khác có reference riêng, không lấy Linux offscreen làm macOS pixel evidence.
6. Light mặc định và dark phải được duyệt. App theo system thì resolver chọn một trong hai token set đã khóa. Focus/hover/disabled/loading/error/selected/active states cũng thuộc reference, không chỉ idle.

Quyền actual browser/native viewer đã bị từ chối ở vòng trước. R00 đang thiếu evidence đó: cần người dùng cấp quyền hoặc kiểm tra/capture thủ công. Không bypass bằng alternate browser, raw CDP, script screenshot hay công cụ OS khác. DOM/model/CLI audit vẫn được dùng để kiểm tra cấu trúc, **không** thay thế visual gate.

## G1 — Fidelity contract: từ CSS sang native

| Thành phần | Reference đã có | Native requirement |
| --- | --- | --- |
| Sidebar | 216 px / 13.5 rem với root 16 px | Shared metric, không tùy screen; section/footer độc lập scrolling đúng design |
| Transcript | Max width 760 px | Center khi không có context; split-pane đúng reference; composer ngoài scroll region |
| Atlas AI Chat | Max width 400 px, inset 18 px | Floating overlay; hide/show không thay map viewport hoặc vị trí node |
| Typography | typography.css: scale 1.15; sidebar -1 px trừ conversation/worktree status | QFont/QML mapping giữ fractional metrics, line-height/weight/tracking; không dùng point size như pixel size |
| Status | Selected neutral surface/border; Working indigo, Completed green + text | Không tái dùng selected=running; reduced motion không mất trạng thái |
| Theme | Két semantic tokens từ pinned CSS + product styles | Shared mapping QPalette/QSS/QML/Scintilla/terminal/preview; không hard-code palette độc lập |
| Connections | connections-page max-width 1170 px **subject to full CSS cascade/available space** | Khóa computed bounding boxes tại G0; không suy max-width là fixed width, không full-screen stretch hoặc co form tùy ý |
| QR/dialog | Pair QR 204 px outer box; buttons/header/Close theo composed styles | Đúng modal bounds, focus trap, Esc/Close semantics; QR thật vẫn scan được sau native rasterization |
| Icons/brand | Lucide 1.40.0; Unplug cho Connections; original brand bytes | Đúng viewBox/stroke/alignment/tint, không emoji/system substitute |
| Editor/terminal | Real CM engine, cùng token và approved chrome | Không dùng static screenshot/text area giả; caret/selection/gutter/wrap thật vẫn đúng reference |

Các số trên chỉ là anchor, **không** thay thế inventory computed geometry của mọi control. R00 xuất design measurements (rect, padding/gap, radius/border, font baseline/line-height, colors, clipping) bằng tooling được phép; R01 tạo native adapter/metrics có test. Khi API public chưa cho phép chỉnh geometry, mở public CM PR; không patch private submodule.

Không copy CSS/SSR runtime làm native architecture. Không convert entire UI sang WebView để dễ giống ảnh. Những phần hợp lý là HTML sẵn (Atlas preview) giữ WebView; editor/transcript vẫn phải đạt performance/native interaction.

**ketjs và ketatlas không được sửa.** Pixel-perfect phải đạt bằng native mapping ở public CM và private product composition/consumer adapters trên public APIs hiện hữu. Không sửa canonical DS CSS/SSR assets, vá viewer internals, fork engine hoặc tự hạ fidelity gate. Nếu một vùng viewer không thể đạt reference với interface được hỗ trợ, R00/R10 phải ghi cụ thể vùng/state, giới hạn API và phương án trong phạm vi để xin quyết định; không đánh dấu pass.

## G2 — Screenshot coverage và acceptance

- [coverage.json](coverage.json) là inventory source: 10 screen / 108 URL / 115 node / 152 edge. Mỗi state có fixture ID ổn định và task mapping; mỗi node dùng lại ảnh state được nhưng **edge interaction** phải có assertion riêng.
- Tại 1440 × 900 / DPR 2: chụp toàn bộ 108 state ở theme khai báo, rồi tương ứng light/dark cho các state chưa có theme đối ứng. Không tính ảnh trùng làm coverage mới.
- Chụp responsive pack 1024 × 768 và 1920 × 1080 cho tất cả screen + mỗi dialog/pane/layout family, thêm longest-title/error/empty states. R00 xác định minimum supported native window; test tại minimum thật, không coi mobile CSS viewport 390 px là desktop production target.
- Typography pack: default 115% đã chọn; min/max setting thực sự được app hỗ trợ, Vietnamese glyphs, long English strings. Không tự thêm localization vào task; bảo toàn string có sẵn và CTA Đăng nhập.
- Interaction pack: keyboard traversal, dialog focus/return, VoiceOver labels, IME, selection/copy, wheel/pinch, reduced motion, resize/font/theme trong lúc streaming. Ảnh đẹp nhưng controls inaccessible không pass.

Artifact mỗi fixture gồm reference.png, native.png, diff.png, alpha overlay, geometry report và evidence JSON (task/screen/flow/node, exact URL/fixture hash, git SHA, pins, environment, dimensions, capture time, reviewer/result). Dữ liệu tổng hợp, không chụp token/QR thật/password/transcript người dùng. Artifact nằm trong private CI/artifact storage hoặc approved private golden folder; không commit captures cá nhân hay paths máy.

**Pass criteria:**

1. Không có missing/extra element, wrong state/copy, clip/overflow, sai wrap/line count/anchor, focus ring mất hoặc icon nhầm.
2. Geometry/spacing/border/radius/type baseline/color khớp locked measurements; **0 sai lệch thiết kế chưa giải thích/phê duyệt**. Không dùng một global percentage (ví dụ 95% giống) để bỏ qua nút lệch hoặc text wrapping sai.
3. Hình raster cùng renderer/environment phải deterministic; pure geometry/color regions không tolerance. Qt vs web có thể khác glyph antialiasing: report diff riêng, thử cùng font/metrics trước; chỉ cho phép mask/tolerance cục bộ do reviewer và designer/user chấp nhận, có reason/version. Không mask toàn bộ text, dialog hoặc WebView.
4. Không hứa bit-identical raster trên mọi OS. Native chrome/system dialog exception nếu cần phải được user chấp thuận rõ, lập reference riêng; không lấy “platform convention” làm miễn trừ ngầm. Nếu chưa được chấp thuận, gate vẫn fail.
5. Regenerate golden là thay design có review; không để cùng patch vừa gây mismatch vừa tự bless expected image. Một screen chỉ Verified khi tất cả recorded states có evidence, không chỉ ảnh entry.

Native pixel test hiện có (docs/agent-workspace.md, streamingPreservesReadingPosition) cho tolerance 2/255 với glyph rounding trong **reading-region stability test**. Test đó không chứng minh design fidelity và không tự áp tolerance này cho toàn redesign. Mở rộng harness riêng cho design/native comparison ở R00/R14.

## G3 — Performance correctness trước optimization

Giữ 4 tầng độc lập: authoritative journal → canonical projection/page → bounded client model → virtualized/lazy render. Virtual ListView/LazyVStack chỉ giải quyết delegates; không tự bound data, Markdown QTextDocument, image decode hay SQLite writes.

| Risk đã thấy trong source | Task / yêu cầu |
| --- | --- |
| Desktop CodexAppServer attach replay history; thread/turns/list facade trả array rỗng | R03/R05: initial page đúng 3 turn, metadata-only background restore; không xem rỗng là hết history |
| Daemon 500 events/768 KiB, cloud cắt suffix 500; snapshot watermark và missing in-turn | R02/R03/R05/R13: negotiated lossless continuation/coverage, bounded oversized event |
| iOS assistantIndexes keyed turnSequence | R13: item identity/order/revision; nhiều assistant/tool item trong cùng turn |
| Mobile loadOlder có async merge không recheck selected epoch; SQLite store @MainActor | R13: scoped operations + actor worker, incremental transaction/checkpoint; UI chỉ publish projection |
| Full-body Markdown parse/layout theo delta, model RAM không bounded | R05/R06: immutable blocks + mutable tail, coalesce delta; parse/media cache có byte budget |
| Backend readTranscriptPage select/filter/sort tất cả retained rows; TTL job full scans | R02: indexed DB paging/expiry batch, explain plan và rows examined; preserve encrypted content opacity |

Coalesce streaming đề xuất 33–50 ms, flush final ngay và giữ event order. Heavy decode/parse/database/index work không chạy trên GUI thread. Sử dụng incremental row change, không full model reset cho mỗi token/page. Stable anchor = item ID + pixel offset + followTail, không numeric index. Latest chủ động fetch newest nếu window đã evict tail, chờ layout settle rồi attach follow-tail; không chỉ scroll tới row cuối hiện đang nạp.

Model budgets đề xuất: active window ≤8 MiB source hoặc 1,000 items (đạt ngưỡng nào trước); tổng source cache tất cả conversations ≤64 MiB; decoded media cache ≤64 MiB; rich layout documents chỉ viewport + overscan; tối đa 8 warm transcript views như cache hiện có. Anchor/selection/in-progress item là pins, cần continuation/block strategy nếu một pin quá lớn; không được lấy lý do pin để RAM vô hạn. Với selection lớn vượt budget: cancelable incremental copy/export từ store, không load toàn history vào UI.

Atlas: active WebView ưu tiên selected tab, inactive giữ lightweight state; index/thumbnail workers concurrency bounded; cache budget đề xuất 64 MiB, không hàng trăm WebViews. Chốt số warm preview/process tối đa sau baseline; report child process RSS riêng, không giấu trong RSS app.

## G4 — Benchmark fixture và target đề xuất

Release build macOS arm64/Metal ở đúng pinned toolchain. Baseline/candidate chạy cùng phần cứng, power/thermal mode và fixture; ≥5 rounds, report median/p95/p99/max phù hợp số samples, cold/warm tách biệt. Không suy p99 latency từ 5 lần launch: launch report từng round/median/max; frame có hàng nghìn samples mới dùng p99. Không đo FPS qua offscreen CI để claim native performance.

| Scenario | Fixture | Target cần khóa ở R00 |
| --- | --- | --- |
| Cold open/restore | 20 conversations, selected có local cache | First painted loading/shell ≤250 ms; actionable shell ≤2 s; first selected cached content ≤1 s sau shell; không parse full 20 histories |
| Warm conversation switch | 100 switches qua 20 conversations | p95 ≤150 ms đến content; draft/selection/anchor không mất |
| Scroll + stream | 1k/10k items, 50 synthetic deltas/s, 60 Hz target | p95 frame ≤16.7 ms, p99 ≤33.3 ms; không GUI stall ≥100 ms; input feedback ≤100 ms |
| Giant turn | >500 events, ≥8 MiB source, single giant item, mixed Markdown/Mermaid/table | Bounded page/decode/layout; không mất item/fragment; interaction giữ budget; progressive render hiển thị trạng thái thật |
| Prepend/reflow/Latest | Page live overlap; width/font/theme/media đổi trong lúc đọc | Anchor drift ≤1 physical px sau settle; không flash/jump sampled frames; Latest thực sự newest/follow-tail |
| Memory plateau | 3 batches ×100 switches, repeated history load/evict, rich previews | Cache counters không vượt budgets; after warm steady-state RSS growth giữa hai batch cuối ≤10%; investigate retained objects nếu không plateau |
| Backend history read | 100k encrypted events/session + expired rows | Số rows đọc/returned bytes bounded theo page; không hydrate full dataset. Controlled local DB p95 ≤200 ms; query plan/index report |
| Retention/job | Fake clock + nhiều page due records | Không trả content sau TTL; batched/resumable cleanup; no scan-all per minute; metrics lag/queue retries có bounds |
| Native iOS | Cùng fixtures, cached/reconnect/stream/prepend/Latest | Physical-device main-thread/frame/memory evidence; simulator chỉ functional, không đại diện hiệu năng iPhone |

Numbers là acceptance proposal, không âm thầm cap/truncate dữ liệu để đạt số. Network/provider latency tách riêng server/render timings; mock deterministic clock không đo real network. Cấm dùng provider quota chỉ để benchmark. Resource usage đo app+daemon+WebView processes, GUI stalls, delegate/layout/document count, parse count/duration, page bytes/latency, DB work, CPU idle/stream.

## G5 — Regression, release và final definition of done

- Theo [testing.md](../../docs/testing.md): focused target cho private module; full suite khi shell/shared header/gitlink đổi; public suite trước private pin; QML/native changes có screenshot ngoài offscreen tests; performance chạy Release/benchmarks.
- Preserve existing ChatMermaid, AgentWorkspaceHistory/Scroll/Session/Interaction/Rendering, Atlas/AtlasWorkspace, RemoteControlView, Settings/Spotlight, daemon protocol/lifecycle/upgrade/desktop-client tests. Bổ sung synthetic fixtures thay vì gọi provider.
- Functional graph coverage gồm primary/recovery/conditional edges; corrupted/expired/missing/out-of-order cases ngoài mock phải được ghi rõ test IDs. Document source-only review không đủ tick pass.
- Archive/delete dùng disposable Git repos và ephemeral journal/DB trong tests, không thao tác current workspace/user sessions. Crash recovery/mixed-version/tombstone-after-24h có tests.
- App restart giữ settings/draft/selected identity đúng migration; session đang chạy không bị kill khi UI close/build upgrade. Restore overlay phải thật sự paint và responsive, không chỉ che GUI freeze.
- Release demo bypass compile-time + runtime opt-in, không xuất hiện trong Release. Logs/captures không credentials, raw user messages hoặc QR secrets. Retention/auth/device revoke staging verification riêng.
- **Hoàn tất refactor** = mọi in-scope screen đã merged implementation + passed functional/API/pixel/performance evidence trên revision cuối; DS/CM/KetAtlas pins đúng; no blocker; user/design sign-off. Không lấy green build làm pixel evidence.
- **TestFlight ready** còn cần staging deployed E2E, secure config/identity/revoke/key lifecycle, Apple signing/entitlements/privacy inputs; cuối cùng mới archive/build/upload iOS theo yêu cầu riêng. PR kế hoạch này không xác nhận đã đạt những điều đó.
