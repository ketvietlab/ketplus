// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

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

    QString initialScreen = QStringLiteral("workspace");
    QString initialState;
    QString initialPanel; // a panel to open: model, mode, limits or reviewer
    qreal initialScroll = -1; // where the transcript starts, 0 (top) to 1 (end); -1 keeps the top

  signals:
    void appearanceChanged();
};
