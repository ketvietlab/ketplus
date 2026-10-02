#include "editor/EditorWidget.h"
#include "preview/MarkdownModeController.h"
namespace ketplus {
void EditorWidget::updateMarkdownPresentation(bool opened) {
    const bool enabled = syntaxName() == QStringLiteral("markdown") && !isLargeFileMode();
    if (enabled && !markdownPresentation_) {
        markdownPresentation_ =
            new MarkdownModeController(this, [this] { return QString::fromUtf8(text()); });
        connect(markdownPresentation_, &MarkdownModeController::fileOpenRequested, this,
                &EditorWidget::previewFileOpenRequested);
        connect(markdownPresentation_, &MarkdownModeController::statusMessageRequested, this,
                &EditorWidget::previewStatusMessageRequested);
        connect(markdownPresentation_, &MarkdownModeController::modeChanged, this,
                &EditorWidget::editorStateChanged);
        markdownPresentation_->setTypography(previewFontSize_, previewLineHeight_);
    }
    if (markdownPresentation_) {
        markdownPresentation_->configure(document().filePath(), presentationPalette_, enabled);
        if (opened && enabled)
            markdownPresentation_->setMode("preview");
    }
}
void EditorWidget::setMarkdownPreviewVisible(bool visible) {
    updateMarkdownPresentation();
    if (markdownPresentation_)
        markdownPresentation_->setMode(visible ? "preview" : "source");
}
bool EditorWidget::isMarkdownPreviewVisible() const {
    return markdownPresentation_ && markdownPresentation_->mode() == "preview" &&
           syntaxName() == "markdown";
}
void EditorWidget::setPreviewTypography(int fontSizePixels, int lineHeightPixels) {
    previewFontSize_ = fontSizePixels;
    previewLineHeight_ = lineHeightPixels;
    if (markdownPresentation_)
        markdownPresentation_->setTypography(fontSizePixels, lineHeightPixels);
}
} // namespace ketplus
