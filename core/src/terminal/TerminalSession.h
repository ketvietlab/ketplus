#pragma once

#include "terminal/PtyProcess.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <vterm.h>

class QTimer;

namespace ketplus {

// Widgets-independent terminal state. It owns the PTY and the VT parser while
// widgets remain responsible only for presentation and user interaction.
class TerminalSession final : public QObject {
    Q_OBJECT

  public:
    explicit TerminalSession(QObject* parent = nullptr);
    ~TerminalSession() override;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] QString shellPath() const;
    [[nodiscard]] int rows() const noexcept;
    [[nodiscard]] int columns() const noexcept;
    [[nodiscard]] int historyLineCount() const noexcept;
    [[nodiscard]] int contentLineCount() const noexcept;
    [[nodiscard]] bool alternateScreen() const noexcept;
    [[nodiscard]] bool synchronizedOutput() const noexcept;
    [[nodiscard]] bool mouseTrackingEnabled() const noexcept;
    [[nodiscard]] VTermPos cursorPosition() const noexcept;
    [[nodiscard]] bool cursorVisible() const noexcept;

    bool start(const QString& workingDirectory);
    bool restart();
    void stop();
    void resizeTerminal(int rows, int columns);
    void reset();
    void clearScrollback();

    void sendKey(VTermKey key, VTermModifier modifiers);
    void sendCodepoint(uint character, VTermModifier modifiers);
    void sendBytes(const QByteArray& bytes);
    void sendPaste(const QString& text);
    void sendMouseMove(int row, int column, VTermModifier modifiers);
    void sendMouseButton(int button, bool pressed, VTermModifier modifiers);
    void setFocused(bool focused);

    // Accepting terminal output is public so a future host can feed replayed or
    // remote bytes without putting transport concerns in the widgets layer.
    void feedOutput(const QByteArray& bytes);

    [[nodiscard]] VTermScreenCell cellAt(int absoluteRow, int column) const;
    [[nodiscard]] QString textForCell(const VTermScreenCell& cell) const;
    [[nodiscard]] QString lineText(int absoluteRow, bool trimTrailing = true) const;
    void setDefaultColors(int foregroundRed, int foregroundGreen, int foregroundBlue,
                          int backgroundRed, int backgroundGreen, int backgroundBlue);
    void convertColorToRgb(VTermColor& color) const;

  signals:
    void inputGenerated(const QByteArray& bytes);
    void outputReceived(const QByteArray& bytes);
    void contentChanged();
    void historyTrimmed(int lineCount);
    void alternateScreenChanged(bool enabled);
    void titleChanged(const QString& title);
    void bellRequested();
    void statusMessageRequested(const QString& message);
    void started();
    void exited(int exitCode);

  private:
    static void outputCallback(const char* bytes, size_t length, void* user);
    static int damageCallback(VTermRect rect, void* user);
    static int cursorCallback(VTermPos position, VTermPos oldPosition, int visible, void* user);
    static int propertyCallback(VTermProp property, VTermValue* value, void* user);
    static int bellCallback(void* user);
    static int resizeCallback(int rows, int columns, void* user);
    static int scrollbackPushCallback(int columns, const VTermScreenCell* cells, void* user);
    static int scrollbackPopCallback(int columns, VTermScreenCell* cells, void* user);
    static int scrollbackClearCallback(void* user);

    void trackSynchronizedOutput(const QByteArray& bytes);
    void emitContentChangedWhenReady();

    PtyProcess process_;
    VTerm* terminal_{nullptr};
    VTermScreen* screen_{nullptr};
    QVector<QVector<VTermScreenCell>> history_;
    int rows_{24};
    int columns_{80};
    VTermPos cursor_{0, 0};
    bool cursorVisible_{true};
    bool alternateScreen_{false};
    int mouseMode_{VTERM_PROP_MOUSE_NONE};
    QString pendingTitle_;
    QString workingDirectory_;
    QTimer* synchronizedOutputTimer_{nullptr};
    bool synchronizedOutput_{false};
    bool contentChangePending_{false};
    QByteArray synchronizedOutputScan_;
};

} // namespace ketplus
