#include "app/SettingsComputerPage.h"
#include "ComputerToolLaunch.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus;
namespace computer = ketplus::computer_tools;

class SettingsComputerPageTest final : public QObject {
    Q_OBJECT

  private slots:
    void missingAgentDisablesTheOptIn() {
        QTemporaryDir root;
        const QString config = root.filePath(QStringLiteral("computer-tools.json"));
        SettingsComputerPage page(config, root.filePath(QStringLiteral("Helpers")));
        auto* enabled = page.findChild<QAbstractButton*>(QStringLiteral("computerEnabled"));
        QVERIFY(enabled);
        QVERIFY(!enabled->isChecked());
        QVERIFY(!enabled->isEnabled());
        QVERIFY(page.findChild<QLabel*>(QStringLiteral("computerStatus"))
                    ->text()
                    .contains("not included"));
    }

    void togglesAndDenyListPersist() {
        QTemporaryDir root;
        const QString helpers = root.filePath(QStringLiteral("Helpers"));
        const QString front = helpers + QStringLiteral("/KetPlus Computer.app/") +
                              QLatin1String(computer::frontExecutable);
        QVERIFY(QDir().mkpath(QFileInfo(front).absolutePath()));
        QFile script(front);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.close();
        QVERIFY(QFile::setPermissions(front, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const QString config = root.filePath(QStringLiteral("computer-tools.json"));

        SettingsComputerPage page(config, helpers);
        auto* enabled = page.findChild<QAbstractButton*>(QStringLiteral("computerEnabled"));
        auto* denied = page.findChild<QLineEdit*>(QStringLiteral("computerDeniedApps"));
        QVERIFY(enabled->isEnabled());
        enabled->setChecked(true);
        QVERIFY(computer::loadSettings(config).enabled);

        denied->setText(QStringLiteral(" com.example.Bank, ,com.example.Mail, com.example.Bank "));
        emit denied->editingFinished();
        QCOMPARE(
            computer::loadSettings(config).deniedApps,
            (QStringList{QStringLiteral("com.example.Bank"), QStringLiteral("com.example.Mail")}));
        QCOMPARE(denied->text(), QStringLiteral("com.example.Bank, com.example.Mail"));

        auto* display = page.findChild<QComboBox*>(QStringLiteral("computerWorkDisplay"));
        QVERIFY(display);
        QCOMPARE(display->currentData().toString(), QStringLiteral("secondary"));
        display->setCurrentIndex(display->findData(QStringLiteral("main")));
        QCOMPARE(computer::loadSettings(config).display, QStringLiteral("main"));

        SettingsComputerPage reopened(config, helpers);
        QVERIFY(
            reopened.findChild<QAbstractButton*>(QStringLiteral("computerEnabled"))->isChecked());
        QCOMPARE(reopened.findChild<QComboBox*>(QStringLiteral("computerWorkDisplay"))
                     ->currentData()
                     .toString(),
                 QStringLiteral("main"));
        enabled->setChecked(false);
        QVERIFY(!computer::loadSettings(config).enabled);
        QCOMPARE(computer::loadSettings(config).deniedApps.size(), 2);
    }

    void failedSaveRevertsTheToggle() {
        QTemporaryDir root;
        const QString helpers = root.filePath(QStringLiteral("Helpers"));
        const QString front = helpers + QStringLiteral("/KetPlus Computer.app/") +
                              QLatin1String(computer::frontExecutable);
        QVERIFY(QDir().mkpath(QFileInfo(front).absolutePath()));
        QFile script(front);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.close();
        QVERIFY(QFile::setPermissions(front, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        // A directory where the file should be: saving fails.
        const QString config = root.filePath(QStringLiteral("blocked"));
        QVERIFY(QDir().mkpath(config));

        SettingsComputerPage page(config, helpers);
        auto* enabled = page.findChild<QAbstractButton*>(QStringLiteral("computerEnabled"));
        enabled->setChecked(true);
        QVERIFY(!enabled->isChecked());
        auto* error = page.findChild<QLabel*>(QStringLiteral("computerSettingsError"));
        QVERIFY(!error->isHidden());
        QVERIFY(!error->text().isEmpty());
    }
};

QTEST_MAIN(SettingsComputerPageTest)
#include "SettingsComputerPageTest.moc"
