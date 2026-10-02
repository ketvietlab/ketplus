#include "preview/MarkdownModeController.h"
#include "preview/MarkdownPreviewPane.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

namespace ketplus {
MarkdownModeController::MarkdownModeController(QWidget* surface, Source source, bool diff)
    : QObject(surface), surface_(surface), bar_(new QWidget(surface)),
      refreshTimer_(new QTimer(this)), source_(std::move(source)), diff_(diff) {
    bar_->setObjectName("markdownModeBar");
    bar_->setAccessibleName("Markdown display mode");
    auto* layout = new QHBoxLayout(bar_);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    const QStringList modes =
        diff ? QStringList{"diff", "preview", "source"} : QStringList{"preview", "source"};
    for (const auto& mode : modes) {
        auto* button = new QToolButton(bar_);
        button->setObjectName("markdownMode-" + mode);
        button->setText(mode.left(1).toUpper() + mode.mid(1));
        button->setAccessibleName("Show Markdown " + mode);
        button->setCheckable(true);
        button->setMinimumHeight(28);
        button->setProperty("mode", mode);
        layout->addWidget(button);
        buttons_.append(button);
        connect(button, &QToolButton::clicked, this, [this, mode] { setMode(mode); });
    }
    bar_->hide();
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(100);
    connect(refreshTimer_, &QTimer::timeout, this, &MarkdownModeController::refresh);
    surface_->installEventFilter(this);
}
void MarkdownModeController::configure(const QString& path, const ThemePalette& palette,
                                       bool enabled) {
    const bool changedFile = path_ != path;
    const bool changed = changedFile || !(palette_ == palette) || enabled_ != enabled;
    if (!changed)
        return;
    path_ = path;
    palette_ = palette;
    enabled_ = enabled;
    bar_->setStyleSheet(
        QStringLiteral("#markdownModeBar { background:%1; border:1px solid %2; border-radius:8px; }"
                       "QToolButton { color:%3; background:transparent; border:0; padding:4px "
                       "12px; border-radius:6px; }"
                       "QToolButton:checked { background:%4; color:%5; }"
                       "QToolButton:focus { border:1px solid %5; }")
            .arg(palette.surfaceRaised, palette.border, palette.textMain, palette.accentSubtle,
                 palette.accent));
    if (!enabled) {
        bar_->hide();
        surface_->setFocusProxy(nullptr);
        if (preview_)
            preview_->hide();
        if (sourceView_)
            sourceView_->hide();
        return;
    }
    dirty_ = true;
    setMode(changedFile || mode_.isEmpty() ? (diff_ ? "diff" : "preview") : mode_);
}
void MarkdownModeController::setMode(const QString& mode) {
    if (!enabled_ || (mode != "preview" && mode != "source" && !(diff_ && mode == "diff")))
        return;
    if (diff_ && mode_ != mode)
        dirty_ = true;
    mode_ = mode;
    if (mode == "preview" && !preview_) {
        preview_ = new MarkdownPreviewPane(surface_);
        preview_->setEmbedded(true);
        preview_->setTypography(fontSize_, lineHeight_);
        connect(preview_, &MarkdownPreviewPane::fileOpenRequested, this,
                &MarkdownModeController::fileOpenRequested);
        connect(preview_, &MarkdownPreviewPane::statusMessageRequested, this,
                &MarkdownModeController::statusMessageRequested);
    }
    if (diff_ && mode == "source" && !sourceView_) {
        sourceView_ = new QPlainTextEdit(surface_);
        sourceView_->setObjectName("markdownDiffSource");
        sourceView_->setReadOnly(true);
    }
    if (preview_)
        preview_->setVisible(mode == "preview");
    if (sourceView_)
        sourceView_->setVisible(mode == "source");
    for (auto* button : buttons_)
        button->setChecked(button->property("mode").toString() == mode);
    surface_->setFocusProxy(mode == "preview"           ? static_cast<QWidget*>(preview_)
                            : diff_ && mode == "source" ? static_cast<QWidget*>(sourceView_)
                                                        : nullptr);
    if (mode == "preview") {
        preview_->raise();
        preview_->setFocus();
    } else if (diff_ && mode == "source") {
        sourceView_->raise();
        sourceView_->setFocus();
    } else
        surface_->setFocus();
    bar_->show();
    place();
    refresh();
    emit modeChanged();
}
void MarkdownModeController::invalidate() {
    dirty_ = true;
    if (enabled_ && (mode_ == "preview" || (diff_ && mode_ == "source")))
        refreshTimer_->start();
}
void MarkdownModeController::setTypography(int fontSize, int lineHeight) {
    fontSize_ = fontSize;
    lineHeight_ = lineHeight;
    if (preview_)
        preview_->setTypography(fontSize, lineHeight);
}
void MarkdownModeController::setAvailable(bool available, const QString& message) {
    available_ = available;
    unavailable_ = message;
    invalidate();
}
void MarkdownModeController::refresh() {
    if (!surface_->isVisible() || !enabled_ || mode_ == "diff" || (!diff_ && mode_ == "source"))
        return;
    if (!dirty_)
        return;
    const auto text = available_ ? source_() : unavailable_;
    if (mode_ == "preview")
        preview_->setSource(text, path_, palette_);
    if (sourceView_ && sourceView_->toPlainText() != text)
        sourceView_->setPlainText(text);
    dirty_ = false;
}
void MarkdownModeController::place() {
    if (preview_)
        preview_->setGeometry(surface_->rect());
    if (sourceView_)
        sourceView_->setGeometry(surface_->rect());
    bar_->adjustSize();
    bar_->move(qMax(8, (surface_->width() - bar_->width()) / 2),
               qMax(8, surface_->height() - bar_->height() - 100));
    bar_->raise();
}
bool MarkdownModeController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == surface_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        place();
        if (event->type() == QEvent::Show)
            refresh();
    }
    return QObject::eventFilter(watched, event);
}
} // namespace ketplus
