#include "AgentWorkspaceViewTest.h"

#include "KetAppearance.h"
#include <QtTest>

void AgentWorkspaceViewTest::init() {
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    ket::Appearance::instance().setZoom(1.0);
}

QTEST_MAIN(AgentWorkspaceViewTest)
