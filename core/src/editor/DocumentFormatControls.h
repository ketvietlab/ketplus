#pragma once

#include "core/Document.h"

#include <QObject>
#include <QString>

#include <functional>

class QMenu;
class QToolButton;
class QWidget;

namespace ketplus {

class EditorWidget;

// The Language, Line Endings and Encoding menus and status-bar buttons of an editor window,
// shared by KétPlus and KétPlus Pro. The window supplies the document they act on and what to
// refresh after a change.
class DocumentFormatControls final : public QObject {
    Q_OBJECT

  public:
    struct Host final {
        // The editor that owns the active document; a split view answers with its source tab.
        std::function<EditorWidget*()> document;
        // After the syntax or the encoding changed: reapply the theme to `editor` and to any
        // view sharing its document.
        std::function<void(EditorWidget* editor)> restyle;
        // After the document was reloaded from disk with another encoding.
        std::function<void(EditorWidget* editor)> reopened;
        // After the syntax changed, which can change how the document previews.
        std::function<void()> syntaxChanged;
        // Saves `editor` in place; returns false when the save failed or was cancelled.
        std::function<bool(EditorWidget* editor)> save;
    };

    DocumentFormatControls(QWidget* window, Host host);

    // Adds the three submenus to `menu`.
    void addMenus(QMenu* menu);
    // Adds the encoding, line-ending and language buttons, created by `makeButton` for each menu.
    void addStatusButtons(const std::function<QToolButton*(QMenu*, const QString&)>& makeButton);

    void updateIndicators();
    void setSyntax(const QString& syntaxName);
    void setLineEnding(LineEnding lineEnding);
    void reopenWithEncoding(TextEncoding encoding);
    void saveWithEncoding(TextEncoding encoding);

  private:
    void syncMenus();

    QWidget* window_{nullptr};
    Host host_;
    QMenu* languageMenu_{nullptr};
    QMenu* lineEndingMenu_{nullptr};
    QMenu* encodingMenu_{nullptr};
    QMenu* saveEncodingMenu_{nullptr};
    QToolButton* encodingButton_{nullptr};
    QToolButton* lineEndingButton_{nullptr};
    QToolButton* languageButton_{nullptr};
};

} // namespace ketplus
