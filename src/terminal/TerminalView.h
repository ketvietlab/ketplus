#pragma once

#include "ui/Theme.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QPoint>
#include <QString>
#include <QVector>

#include <vterm.h>

class QContextMenuEvent;
class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QWheelEvent;
class QWidget;

namespace ketplus {

class TerminalSession;

class TerminalView final : public QAbstractScrollArea {
    Q_OBJECT

  public:
    explicit TerminalView(TerminalSession& session, QWidget* parent = nullptr);

    void applyTheme(const ThemePalette& palette);
    void setTypography(int fontSizePixels, int lineHeightPixels);
    void start(const QString& workingDirectory);
    bool restart();
    void pasteClipboard();
    void copySelection();
    void selectAll();
    void clearSelection();
    void clearScrollback();
    void openSearch();

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QString visibleText() const;
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool hasSelection() const noexcept;

  signals:
    void titleChanged(const QString& title);
    void statusMessageRequested(const QString& message);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    struct CellPosition final {
        int row{0};
        int column{0};
    };

    struct SearchMatch final {
        int row{0};
        int firstColumn{0};
        int lastColumn{0};
    };

    void requestRender();
    void updateGeometryFromViewport();
    void updateScrollBar(bool preserveBottom);
    void updateSearchGeometry();
    void rebuildSearchMatches();
    void activateSearchMatch(int index);
    void findNext(bool backwards);
    void closeSearch();
    void adjustSelectionForTrimmedHistory(int lineCount);
    [[nodiscard]] VTermModifier modifiersFor(QKeyEvent* event) const;
    [[nodiscard]] VTermModifier modifiersFor(Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] QColor colorFor(VTermColor color, bool foreground) const;
    [[nodiscard]] VTermScreenCell cellAtVisiblePosition(int displayRow, int column) const;
    [[nodiscard]] int historyOffset() const;
    [[nodiscard]] CellPosition positionForPoint(const QPoint& point) const;
    [[nodiscard]] CellPosition viewportPositionForPoint(const QPoint& point) const;
    [[nodiscard]] bool cellIsSelected(int absoluteRow, int column) const;
    [[nodiscard]] int searchMatchAt(int absoluteRow, int column) const;
    [[nodiscard]] static bool positionLess(CellPosition left, CellPosition right) noexcept;

    TerminalSession& session_;
    int cellWidth_{8};
    int cellHeight_{17};
    int lineHeightPixels_{20};
    int leftPadding_{10};
    int topPadding_{8};
    bool cursorBlinkOn_{true};
    QColor foreground_{QStringLiteral("#CDD2D8")};
    QColor background_{QStringLiteral("#171B20")};
    QColor cursorColor_{QStringLiteral("#5968DF")};
    QColor selectionColor_{QStringLiteral("#394B8A")};
    QColor searchColor_{QStringLiteral("#8A6D1E")};
    QColor currentSearchColor_{QStringLiteral("#B98900")};
    QString preeditText_;
    int preeditCursor_{0};
    QTimer* cursorTimer_{nullptr};
    QTimer* renderTimer_{nullptr};
    QTimer* searchTimer_{nullptr};
    bool renderPending_{false};
    bool selecting_{false};
    bool selectionActive_{false};
    CellPosition selectionAnchor_;
    CellPosition selectionHead_;
    QWidget* searchBar_{nullptr};
    QLineEdit* searchEdit_{nullptr};
    QLabel* searchStatus_{nullptr};
    QVector<SearchMatch> searchMatches_;
    int currentSearchMatch_{-1};
};

} // namespace ketplus
