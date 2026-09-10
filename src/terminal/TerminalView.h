#pragma once

#include "ui/Theme.h"

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>
#include <QPoint>
#include <QString>
#include <QVector>

#include <vterm.h>

class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;

namespace ketplus {

class PtyProcess;

class TerminalView final : public QAbstractScrollArea {
    Q_OBJECT

  public:
    explicit TerminalView(PtyProcess& process, QWidget* parent = nullptr);
    ~TerminalView() override;

    void applyTheme(const ThemePalette& palette);
    void start(const QString& workingDirectory);
    void pasteClipboard();

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QString visibleText() const;

  signals:
    void titleChanged(const QString& title);
    void statusMessageRequested(const QString& message);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    static void outputCallback(const char* bytes, size_t length, void* user);
    static int damageCallback(VTermRect rect, void* user);
    static int cursorCallback(VTermPos position, VTermPos oldPosition, int visible,
                              void* user);
    static int propertyCallback(VTermProp property, VTermValue* value, void* user);
    static int bellCallback(void* user);
    static int resizeCallback(int rows, int columns, void* user);
    static int scrollbackPushCallback(int columns, const VTermScreenCell* cells,
                                      void* user);
    static int scrollbackPopCallback(int columns, VTermScreenCell* cells, void* user);
    static int scrollbackClearCallback(void* user);

    void consumeOutput(const QByteArray& bytes);
    void trackSynchronizedOutput(const QByteArray& bytes);
    void requestRender();
    void updateGeometryFromViewport();
    void updateScrollBar(bool preserveBottom);
    void sendKey(VTermKey key, VTermModifier modifiers);
    [[nodiscard]] VTermModifier modifiersFor(QKeyEvent* event) const;
    [[nodiscard]] QColor colorFor(VTermColor color, bool foreground) const;
    [[nodiscard]] VTermScreenCell cellAtVisiblePosition(int displayRow, int column) const;
    [[nodiscard]] QString textForCell(const VTermScreenCell& cell) const;
    [[nodiscard]] int historyOffset() const;

    PtyProcess& process_;
    VTerm* terminal_{nullptr};
    VTermScreen* screen_{nullptr};
    QVector<QVector<VTermScreenCell>> history_;
    int rows_{24};
    int columns_{80};
    int cellWidth_{8};
    int cellHeight_{17};
    int leftPadding_{10};
    int topPadding_{8};
    VTermPos cursor_{0, 0};
    bool cursorVisible_{true};
    bool cursorBlinkOn_{true};
    bool alternateScreen_{false};
    QColor foreground_{QStringLiteral("#CDD2D8")};
    QColor background_{QStringLiteral("#171B20")};
    QColor cursorColor_{QStringLiteral("#5968DF")};
    QString pendingTitle_;
    QString preeditText_;
    int preeditCursor_{0};
    QTimer* cursorTimer_{nullptr};
    QTimer* renderTimer_{nullptr};
    QTimer* synchronizedOutputTimer_{nullptr};
    bool renderPending_{false};
    bool synchronizedOutput_{false};
    QByteArray synchronizedOutputScan_;
};

} // namespace ketplus
