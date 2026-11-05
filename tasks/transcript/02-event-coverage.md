# Transcript V2 — event contract và coverage ledger

Baseline triển khai: `ketplus-desktop` tại `8d464fbc2fe9` trên nhánh
`feat/worktree-base-and-delete`. Ledger phải được audit lại khi baseline thay đổi.

## 1. Nguyên tắc

Nguồn sự thật là event có kiểu, không phải chuỗi đã format cho UI. Daemon tiếp tục lưu item/protocol
event; Desktop tạo projection typed để render. Presentation grouping không được ghi ngược vào journal.

```text
Daemon journal / app-server item
            ↓
ActivityNormalizer — phân loại + sanitize + giữ raw identity
            ↓
TranscriptEventModel — một canonical presentation model
            ↓
ActivityGrouper — group dẫn xuất, có thể tái tạo
            ↓
ActivityRendererRegistry — renderer chuyên biệt hoặc generic fallback
```

## 2. Canonical `TranscriptEvent`

Tên trường có thể đổi trong implementation, nhưng semantic tối thiểu phải giữ:

| Trường | Ý nghĩa |
|---|---|
| `key` | Identity ổn định gồm session/generation/turn/item/attempt |
| `turnKey` | Ranh giới không được group xuyên qua |
| `sequence` | Thứ tự canonical, không phụ thuộc thời điểm thumbnail/load UI xong |
| `category` | `user`, `activity`, `final`, `question`, `error`, `system` |
| `type` / `subtype` | Loại gốc và semantic đã normalize |
| `phase` | started, delta, completed, failed, interrupted |
| `title` / `summary` | Text trình bày ngắn, không phải raw source of truth |
| `target` | File, command, URL, asset, app hoặc tool liên quan |
| `details` | Payload typed cho renderer chuyên biệt |
| `sanitizedPayload` | Payload có thể mở trong generic detail |
| `running` / `status` | Trạng thái lifecycle |
| `occurredAt` | Timestamp nếu nguồn có cung cấp |
| `groupHint` | Gợi ý group, không quyết định storage |

Normalizer phải deterministic: cùng raw event corpus phải tạo cùng sequence canonical ở live và
replay.

## 3. Coverage ledger bắt buộc

| Nguồn/event quan sát được | Canonical subtype | UI chính | Detail bắt buộc | Test |
|---|---|---|---|---|
| `userMessage` | `user.message` | User prompt | attachment list | live + replay |
| `agentMessage` final | `assistant.final` | Final answer | copy/open full | streaming + replay |
| `agentMessage` commentary | `assistant.commentary` | Assistant progress prose | lossless body | live + replay |
| `reasoning` | `assistant.reasoning` | Không render | không expose | no-leak |
| `commandExecution` | `command.run` | Running/Ran exact command | cwd, exit, output preview | success/fail/interrupted |
| `fileChange` | `file.change` | Created/Edited/Deleted N files | exact paths, stats/diff link | single + grouped |
| file/tool read | `file.read` | Read file(s) | exact paths | single + grouped |
| code/file search | `search.code` | Searched code/files | query, scope | empty + results |
| `webSearch` | `search.web` | Searched the web | query, sources | live + replay |
| browser/computer open | `browser.open` | Opened website | title, redacted URL | normal + secret URL |
| browser navigate/click | `browser.navigate` / `browser.click` | Navigated/Clicked | target, page | ordered sequence |
| browser screenshot | `browser.screenshot` | Captured screenshot | thumbnail, source page | preview |
| image view tool | `image.view` | Viewed 1/N images | complete gallery | 1, 4, 50 images |
| `mcpToolCall` | `integration.mcp` | Used server/tool | sanitized args/result | success/fail |
| `dynamicToolCall` | `tool.dynamic` | Used tool | sanitized args/result | success/fail |
| `collabAgentToolCall` | `integration.collaboration` | Used a collaborator | sanitized payload | live + replay |
| `subAgentActivity` | `integration.subagent` | Used a sub-agent | sanitized payload | live + replay |
| `sleep` | `activity.wait` | Waiting/Waited | sanitized payload | live + replay |
| app/connector call | `integration.app` | Used app/integration | app, operation | grouped mix |
| generated/opened artifact | `artifact.create/open` | Created/Opened artifact | path/URL/type | click behavior |
| approval request/result | `approval.request/result` | Approval card/result | scope + choice | accept/deny/replay |
| structured question | `question.request/result` | Question card/result | options + answer | pending/replay |
| `contextCompaction` | `system.compaction` | Compact system row hoặc ẩn theo policy | status | replay |
| turn/session error | `error.turn/activity` | Error row | safe message + related key | retry |
| event chưa biết | `unknown` | Generic activity row | type + sanitized payload | never-drop fixture |

Ledger này phải được cập nhật từ schema và corpus, không phải từ trí nhớ. Nếu một raw `type/subtype`
không có dòng tương ứng thì CI coverage test phải đỏ.

## 4. Renderer registry

Không đặt một `switch` khổng lồ trong `AgentWorkspace.qml`. Registry ánh xạ semantic subtype sang
component:

```text
file.*          → FileActivityRow
command.*       → CommandActivityRow
search.*        → SearchActivityRow
browser.*       → BrowserActivityRow
image.view      → ImageActivityRow / ImageGalleryDetail
integration.*  → IntegrationActivityRow
approval.*      → ApprovalActivityCard
question.*      → QuestionActivityCard
error.*         → ErrorActivityRow
*               → GenericActivityRow
```

Registry phải có test chứng minh subtype không biết luôn chọn `GenericActivityRow`.

## 5. Quy tắc đặc biệt cho ảnh

- Normalizer nhận array asset refs, không nhận một chuỗi label đã gộp.
- Count lấy từ array canonical, không lấy từ title.
- 1 ảnh dùng singular; N ảnh dùng plural và đúng số lượng.
- Chỉ materialize thumbnail đang/ gần viewport; giới hạn cache theo byte và item count.
- 50 ảnh mặc định chỉ render một số thumbnail bounded, có `Show all`; model vẫn giữ đủ 50 refs.
- Asset hỏng hiển thị placeholder cùng filename/error; không làm biến mất cả activity.

## 6. Quy tắc đặc biệt cho browser/website

- Tách URL thành display origin và sanitized full URL.
- Redact tối thiểu các query/fragment mang `token`, `key`, `secret`, `password`, `auth`, `code` và
  credential trong authority.
- Title do trang cung cấp là untrusted text; render như text, không parse HTML/Markdown.
- Replay không được navigate. Chỉ click chủ động của người dùng mới mở trang.
- Chuỗi `open → navigate → click → screenshot` giữ đúng sequence và có thể group dưới một browser run.

## 7. Inventory đã kiểm chứng

Inventory ngày 2026-09-13 được lấy từ daemon journal thật trên máy phát triển. Chỉ aggregate count,
raw type, item type và phase được giữ; nội dung người dùng và payload value không được đưa vào repo.
Fixture nguồn: [`observed-inventory.json`](../../tests/fixtures/transcript-v2/observed-inventory.json).

Các protocol type quan sát được gồm approval lifecycle, assistant/command delta, item lifecycle,
runtime error, session lifecycle, turn lifecycle và user message. Các item type quan sát được gồm:

- `agentMessage` với phase `commentary` và `final_answer`;
- `collabAgentToolCall`, `subAgentActivity` và `sleep`;
- `commandExecution`, `fileChange`, `webSearch` và `contextCompaction`;
- `imageView`, `imageGeneration`, `mcpToolCall`, `reasoning` và `userMessage`.

`ActivityNormalizer::registeredProtocolEventTypes()` và `registeredRawTypes()` là coverage registry.
Test `coversVersionedTranscriptCorpus` so inventory với registry; protocol chưa biết nhận explicit
generic policy và item chưa biết luôn tạo visible generic event.

Giới hạn của inventory này:

1. Aggregate inventory chứng minh loại event đã xuất hiện, không lưu nội dung session thật.
2. Payload test dùng shape đã sanitize và giá trị tổng hợp để không commit dữ liệu người dùng.
3. Remote privacy và large-event behavior vẫn thuộc gate PR 5, không được coi là hoàn tất ở PR 1.

## 8. Corpus chống bỏ sót

Corpus versioned hiện nằm tại `tests/fixtures/transcript-v2/`:

- [`canonical-items.json`](../../tests/fixtures/transcript-v2/canonical-items.json): item types, phase
  visibility, semantic event type, unknown fallback và secret payload.
- [`protocol-events.json`](../../tests/fixtures/transcript-v2/protocol-events.json): policy cho lifecycle,
  delta, approval, runtime error, metadata và future protocol event.
- [`observed-inventory.json`](../../tests/fixtures/transcript-v2/observed-inventory.json): aggregate
  protocol/item inventory từ journal thật.

`mergesLiveAndReplayTranscriptLifecycle` kiểm start/delta/complete, failed attempt và retry attempt.
`coversVersionedTranscriptCorpus` feed toàn canonical corpus theo cả đường live và replay rồi so toàn
bộ canonical output. Các case 1/4/50/broken image, restart daemon và remote vẫn có fixture/test riêng
ở các wave tương ứng vì chúng cần asset resolver và integration boundary, không chỉ normalizer.

Mỗi bug production về transcript phải thêm fixture tái hiện trước khi sửa.
