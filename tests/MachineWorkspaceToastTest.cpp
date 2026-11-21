#include "KetAppearance.h"
#include "KetToast.h"
#include "agent/ToastAvoidanceOwner.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineWorkspaceView.h"
#include "ui/Theme.h"

#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

using namespace ketplus;

class MachineWorkspaceToastTest final : public QObject {
    Q_OBJECT

  private slots:
    void composerReservation_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void composerReservation() {
        QFETCH(bool, dark);
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QVERIFY(QSettings().fileName().startsWith(settings.path() + "/"));
        QVERIFY(!QSettings().contains(QStringLiteral("workspace/lastRoot")));
        ThemeManager theme(*qApp);
        const auto restoreTheme = qScopeGuard([&] { theme.setMode(ThemeManager::Mode::Light); });
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        MachineCatalog catalog;
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* view = new MachineWorkspaceView(&catalog, &window);
        layout->addWidget(view);
        auto* composer = view->findChild<QPlainTextEdit*>(QStringLiteral("machineComposer"));
        QVERIFY(composer);
        composer->setFixedHeight(48);
        window.resize(1000, 760);
        window.show();
        ket::Toast toast(QStringLiteral("Synthetic status"), &window, ket::ToastTone::Neutral);
        toast.popup(0);
        const int gap = qRound(ket::Appearance::instance().scaled(toast.recipe().bottomOffset));
        const auto top = [&] { return composer->mapTo(&window, QPoint()).y(); };
        QTRY_COMPARE(toast.y(), top() - gap - toast.height());
        const int beforeGrowth = toast.y();
        composer->setFixedHeight(120);
        QTRY_VERIFY(toast.y() < beforeGrowth);
        QTRY_COMPARE(toast.y(), top() - gap - toast.height());
        window.resize(1100, 860);
        QTRY_COMPARE(toast.y(), top() - gap - toast.height());

        QWidget baselineWindow;
        baselineWindow.resize(window.size());
        ket::Toast baseline(QStringLiteral("Synthetic status"), &baselineWindow,
                            ket::ToastTone::Neutral);
        baseline.popup(0);
        // Default placement includes the kit's shadow margin; compare the real baseline.
        // The transcript-only AgentWorkspace nested in this view must not clear its owner.
        QTRY_COMPARE(window.property(toast_avoidance::ownerProperty).value<QObject*>(), view);
        composer->hide();
        QTRY_COMPARE(toast.geometry(), baseline.geometry());
        composer->show();
        QTRY_COMPARE(toast.y(), top() - gap - toast.height());
        view->hide();
        QTRY_COMPARE(toast.geometry(), baseline.geometry());
        view->show();
        QTRY_COMPARE(toast.y(), top() - gap - toast.height());

        // An inactive Machine view cannot clear a newer reservation from another composer.
        QObject otherOwner;
        window.setProperty(toast_avoidance::ownerProperty,
                           QVariant::fromValue<QObject*>(&otherOwner));
        ket::Toast::setBottomAvoidance(&window, 80);
        view->hide();
        QCOMPARE(window.property(toast_avoidance::ownerProperty).value<QObject*>(), &otherOwner);
        QCOMPARE(toast.y(), window.height() - 80 - gap - toast.height());
        delete view;
        QCOMPARE(toast.y(), window.height() - 80 - gap - toast.height());
        window.setProperty(toast_avoidance::ownerProperty, QVariant());
        ket::Toast::setBottomAvoidance(&window, 0);
    }
};
QTEST_MAIN(MachineWorkspaceToastTest)
#include "MachineWorkspaceToastTest.moc"
