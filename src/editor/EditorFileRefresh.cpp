#include "editor/EditorWidget.h"

#include <ScintillaMessages.h>

namespace ketplus {
namespace {
constexpr unsigned int message(Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}
} // namespace

Document::Result EditorWidget::refreshFromDisk() {
    if (document_->isUntitled() || document_->isModified())
        return {true, {}};
    if (hibernated_)
        return restoreFromDisk();

    // Read before changing the live document. Size/mtime alone miss quick equal-size edits.
    Document latest;
    const auto loaded = latest.load(document_->filePath(), document_->textEncoding());
    if (!loaded.ok)
        return {false, loaded.error};
    if (loaded.content == text())
        return {true, {}}; // Preserve undo/redo and selections when nothing changed.

    const ViewState state{
        .caret = send(message(Scintilla::Message::GetCurrentPos)),
        .anchor = send(message(Scintilla::Message::GetAnchor)),
        .firstVisibleLine = send(message(Scintilla::Message::GetFirstVisibleLine)),
        .horizontalOffset = send(message(Scintilla::Message::GetXOffset)),
    };
    *document_ = std::move(latest); // Keep the shared Document identity for split views.
    setText(loaded.content);
    configureLexerForPath(document_->filePath());
    restoreViewState(state);
    updateMarkdownPresentation();
    return {true, {}};
}
} // namespace ketplus
