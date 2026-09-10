# KetPlus Markdown Preview

File này dùng để kiểm tra Markdown preview, Mermaid và theme của KetPlus.

> Mở file rồi nhấn **Cmd/Ctrl + Shift + V** để bật preview.
> Click vào một sơ đồ Mermaid để mở popup; cuộn để zoom và kéo để pan.

## Nội dung cơ bản

KetPlus hỗ trợ **bold**, *italic*, ~~strikethrough~~, `inline code` và
[liên kết tới phần Mermaid](#mermaid).

### Checklist

- [x] Markdown preview từ buffer chưa lưu
- [x] Theme sáng và tối
- [x] Ảnh và link tương đối
- [x] Mermaid qua `mmdc`
- [ ] Scroll sync theo dòng

### Bảng

| Thành phần | Trạng thái | Ghi chú |
|---|:---:|---|
| Markdown | ✅ | Render native bằng Qt |
| Mermaid | ✅ | Render SVG bằng `mmdc` |
| Zoom/Pan | ✅ | Mở bằng popup riêng |

### Ảnh tương đối

![KetPlus logo](../assets/brand/ketplus-mark.png)

## Code

```cpp
#include <iostream>

int main() {
    std::cout << "Hello from KetPlus!\n";
    return 0;
}
```

```json
{
  "editor": "KetPlus",
  "preview": true,
  "languages": ["Markdown", "Mermaid"]
}
```

## Mermaid

### Flowchart

```mermaid
flowchart LR
    A[Open Markdown] --> B{mmdc installed?}
    B -- Yes --> C[Render SVG]
    B -- No --> D[Show install notice]
    C --> E[Inline preview]
    E --> F[Click diagram]
    F --> G[Popup with zoom and pan]
```

### Sequence diagram

```mermaid
sequenceDiagram
    actor User
    participant Editor as KetPlus Editor
    participant Preview as Markdown Preview
    participant MMDC as mmdc

    User->>Editor: Edit Mermaid block
    Editor->>Preview: Buffer changed
    Preview->>Preview: Debounce 150 ms
    Preview->>MMDC: Render diagram
    MMDC-->>Preview: SVG
    Preview-->>User: Updated preview
```

### State diagram

```mermaid
stateDiagram-v2
    [*] --> Hidden
    Hidden --> Visible: Cmd/Ctrl + Shift + V
    Visible --> Rendering: Document changed
    Rendering --> Ready: SVG generated
    Rendering --> Missing: mmdc not found
    Ready --> Popup: Click diagram
    Popup --> Ready: Close popup
    Visible --> Hidden: Close preview
```

---

If Mermaid does not render, install the official CLI:

```sh
npm install -g @mermaid-js/mermaid-cli
```
