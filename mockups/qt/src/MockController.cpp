// SPDX-License-Identifier: MPL-2.0
#include "MockController.h"

#include "KetAppearance.h"
#include "KetPrint.h"

#include <QFile>

using ket::Appearance;

MockController::MockController(QObject* parent) : QObject(parent) {
    if (!teamCatalog_.load(QStringLiteral(":/mock/i18n/ketplus_teams_vi.qm")) ||
        !mockTeamCatalog_.load(QStringLiteral(":/mock/i18n/mock_teams_vi.qm")))
        qWarning("QML mock team translation catalogs could not be loaded");
    connect(&Appearance::instance(), &Appearance::themeChanged, this,
            &MockController::appearanceChanged);
    connect(&Appearance::instance(), &Appearance::zoomChanged, this,
            &MockController::appearanceChanged);
}

bool MockController::dark() const { return Appearance::instance().theme() == ket::Theme::Dark; }

bool MockController::followSystem() const {
    return Appearance::instance().mode() == Appearance::Mode::System;
}

qreal MockController::zoom() const { return Appearance::instance().zoom(); }

void MockController::setDark(bool dark) {
    Appearance::instance().setMode(dark ? Appearance::Mode::Dark : Appearance::Mode::Light);
    emit appearanceChanged();
}

void MockController::setFollowSystem(bool follow) {
    Appearance::instance().setMode(
        follow ? Appearance::Mode::System
               : (dark() ? Appearance::Mode::Dark : Appearance::Mode::Light));
    emit appearanceChanged();
}

void MockController::setZoom(qreal zoom) { Appearance::instance().setZoom(zoom); }

QString MockController::resourceText(const QString& path) const {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString MockController::teamText(const QString& language, const QString& source) const {
    if (language != QLatin1String("vi"))
        return source;
    const QByteArray utf8 = source.toUtf8();
    // Product wording wins. The second catalog contains only mock-specific copy.
    QString translated = teamCatalog_.translate("Team", utf8.constData());
    if (translated.isEmpty())
        translated = mockTeamCatalog_.translate("Team", utf8.constData());
    return translated.isEmpty() ? source : translated;
}

bool MockController::printImage(const QUrl& source, int rotation, const QString& title) const {
    return ket::printImage(source, rotation, title);
}
