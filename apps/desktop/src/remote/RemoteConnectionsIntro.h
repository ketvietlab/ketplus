#pragma once

#include <QColor>
#include <QWidget>

class QBoxLayout;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;

namespace ketplus {

class ConnectionsScene;

// Connections before sign-in (#177, mockups/qt ConnectionsIntro): what connecting gives, this
// Mac's Agent Workspace and KetPlus Mobile answering the same question, the three pairing steps
// and who stays in control.
class RemoteConnectionsIntro final : public QWidget {
    Q_OBJECT

  public:
    // The view's palette roles the scene paints with; the labels follow the view's stylesheet.
    struct SceneColors {
        QColor glow, page, border, strong, panel, muted;
        bool dark{false};
    };

    explicit RemoteConnectionsIntro(QWidget* parent = nullptr);
    [[nodiscard]] QPushButton* signInButton() const { return signIn_; }
    // Reasons from the daemon, shown under the header.
    [[nodiscard]] QLabel* notice() const { return notice_; }
    void setExpired(bool expired);
    void setSceneColors(const SceneColors& colors);

  signals:
    void signInRequested();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    friend class RemoteControlViewTest;
    void applyWidth();

    QFrame* expired_{nullptr};
    QLabel* notice_{nullptr};
    QPushButton* signIn_{nullptr};
    QBoxLayout* hero_{nullptr};
    QWidget* copy_{nullptr};
    QLabel* heroText_{nullptr};
    ConnectionsScene* scene_{nullptr};
    QGridLayout* stepsHeader_{nullptr};
    QLabel* stepsNote_{nullptr};
    QGridLayout* steps_{nullptr};
    QList<QWidget*> stepCells_;
    bool narrow_{false};
    bool laidOut_{false};
};

} // namespace ketplus
