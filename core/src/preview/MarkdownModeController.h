#pragma once

#include "ui/Theme.h"
#include <QObject>
#include <functional>

class QWidget;
class QToolButton;
class QPlainTextEdit;
class QTimer;
namespace ketplus {
class MarkdownPreviewPane;

// Presentation only: the host owns the source document and its save/undo state.
class MarkdownModeController final : public QObject {
    Q_OBJECT
  public:
    using Source = std::function<QString()>;
    MarkdownModeController(QWidget* surface, Source source, bool diff = false);
    void configure(const QString& path, const ThemePalette& palette, bool enabled);
    void setMode(const QString& mode);
    QString mode() const { return mode_; }
    void invalidate();
    void setTypography(int fontSize, int lineHeight);
    void setAvailable(bool available, const QString& message = {});
  signals:
    void fileOpenRequested(const QString& path);
    void statusMessageRequested(const QString& message);
    void modeChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void refresh();
    void place();
    QWidget* surface_;
    QWidget* bar_;
    MarkdownPreviewPane* preview_{nullptr};
    QPlainTextEdit* sourceView_{nullptr};
    QTimer* refreshTimer_;
    QList<QToolButton*> buttons_;
    Source source_;
    ThemePalette palette_;
    QString path_;
    QString mode_;
    QString unavailable_;
    bool diff_{false};
    bool enabled_{false};
    bool available_{true};
    bool dirty_{true};
    int fontSize_{14};
    int lineHeight_{22};
};
} // namespace ketplus
