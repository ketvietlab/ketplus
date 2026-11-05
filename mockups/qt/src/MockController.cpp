// SPDX-License-Identifier: MPL-2.0
#include "MockController.h"

#include "KetAppearance.h"

using ket::Appearance;

MockController::MockController(QObject* parent) : QObject(parent) {
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
