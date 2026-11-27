// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <QTranslator>
#include <QUrl>

// The mock's link to the kit's appearance: QML reads KetTheme, and changes the theme or the
// zoom through this object (QML singleton `Mock` in KetPlusMock.Native).
class MockController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dark READ dark NOTIFY appearanceChanged)
    Q_PROPERTY(bool followSystem READ followSystem NOTIFY appearanceChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY appearanceChanged)
    Q_PROPERTY(QString initialScreen MEMBER initialScreen CONSTANT)
    Q_PROPERTY(QString initialState MEMBER initialState CONSTANT)
    Q_PROPERTY(QString initialPanel MEMBER initialPanel CONSTANT)
    Q_PROPERTY(qreal initialScroll MEMBER initialScroll CONSTANT)
    Q_PROPERTY(QString edition MEMBER edition CONSTANT)
    Q_PROPERTY(QString chrome MEMBER chrome CONSTANT)

  public:
    explicit MockController(QObject* parent = nullptr);

    [[nodiscard]] bool dark() const;
    [[nodiscard]] bool followSystem() const;
    [[nodiscard]] qreal zoom() const;

    Q_INVOKABLE void setDark(bool dark);
    Q_INVOKABLE void setFollowSystem(bool follow);
    Q_INVOKABLE void setZoom(qreal zoom);
    // A bundled text resource, such as the repository document the editor previews.
    Q_INVOKABLE QString resourceText(const QString& path) const;
    // Team-only copy; the rest of the mock keeps the application's English chrome.
    Q_INVOKABLE QString teamText(const QString& language, const QString& source) const;
    // The image viewer's Print: the system print dialog, as the app opens it.
    Q_INVOKABLE bool printImage(const QUrl& source, int rotation, const QString& title) const;

    QString initialScreen = QStringLiteral("workspace");
    QString initialState;
    QString
        initialPanel; // a panel to open: model, mode, limits, reviewer, composer, images or mermaid
    qreal initialScroll = -1; // where the transcript starts, 0 (top) to 1 (end); -1 keeps the top
    QString edition = QStringLiteral("pro"); // "pro" (Main.qml) or "lite" (LiteMain.qml)
    // Window chrome of the Lite window: "macos" (the system menu bar), "windows" or "linux"
    // (a title bar and a menu bar drawn in the window).
    QString chrome = QStringLiteral("macos");

  signals:
    void appearanceChanged();

  private:
    QTranslator teamCatalog_;
    QTranslator mockTeamCatalog_;
};
