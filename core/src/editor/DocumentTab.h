#pragma once

#include <QIcon>
#include <QString>

namespace ketplus {

class EditorWidget;

// An editor tab's label and icon, the same in the main tabs and the split pane: the
// document name with a bullet while modified, and the mock's file-text icon for Markdown
// and plain text or file-code for other files.
[[nodiscard]] QString documentTabTitle(const EditorWidget* editor);
[[nodiscard]] QIcon documentTabIcon(const EditorWidget* editor);

} // namespace ketplus
