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

  public:
    explicit MockController(QObject* parent = nullptr);

    [[nodiscard]] bool dark() const;
    [[nodiscard]] bool followSystem() const;
    [[nodiscard]] qreal zoom() const;

    Q_INVOKABLE void setDark(bool dark);
    Q_INVOKABLE void setFollowSystem(bool follow);
    Q_INVOKABLE void setZoom(qreal zoom);

    QString initialScreen = QStringLiteral("workspace");
    QString initialState;

  signals:
    void appearanceChanged();
};
