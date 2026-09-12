#include "app/SettingsDialog.h"
#include "git/GitParser.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <initializer_list>
#include <utility>

namespace {

QByteArray nulSeparated(const std::initializer_list<QByteArray>& fields) {
    QByteArray output;
    for (const auto& field : fields) {
        output.append(field);
        output.append('\0');
    }
    return output;
}

class SyntheticSettingsPage final : public ketplus::SettingsPage {
  public:
    SyntheticSettingsPage(QString id, QString title, QSettings& storage, QString storageKey,
                          int defaultValue = 0)
        : SettingsPage(std::move(id), std::move(title)), storage_(storage),
          storageKey_(std::move(storageKey)), defaultValue_(defaultValue),
          persistedValue_(storage_.value(storageKey_, defaultValue_).toInt()),
          bufferedValue_(persistedValue_) {}

    [[nodiscard]] bool hasChanges() const override { return bufferedValue_ != persistedValue_; }

    void resetToDefaults() override {
        ++resetCount;
        bufferedValue_ = defaultValue_;
        emit changed();
    }

    void apply() override {
        ++applyCount;
        persistedValue_ = bufferedValue_;
        storage_.setValue(storageKey_, persistedValue_);
        storage_.sync();
    }

    void setBufferedValue(const int value) {
        bufferedValue_ = value;
        emit changed();
    }

    [[nodiscard]] int bufferedValue() const { return bufferedValue_; }

    int resetCount{0};
    int applyCount{0};

  private:
    QSettings& storage_;
    QString storageKey_;
    int defaultValue_{0};
    int persistedValue_{0};
    int bufferedValue_{0};
};

QPushButton* findButton(ketplus::SettingsDialog& dialog,
                        const QDialogButtonBox::StandardButton role,
                        const QString& fallbackRole = {}) {
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    if (buttons == nullptr) {
        return nullptr;
    }
    if (auto* button = buttons->button(role); button != nullptr) {
        return button;
    }
    for (auto* button : buttons->findChildren<QPushButton*>()) {
        if (!fallbackRole.isEmpty() && button->property("kvRole") == fallbackRole) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class RefactorWave1ContractTest final : public QObject {
    Q_OBJECT

  private slots:
    void cancelDoesNotApplyOrPersistExtensionState();
    void resetBuffersExtensionDefaultsUntilSave();
    void themePreviewRollsBackOnCancel();
    void saveCurrentlyAppliesEveryExtensionPage();
    void currentNativePaletteBaseline();
    void parserReadsObservedWorktreeFlags();
};

void RefactorWave1ContractTest::cancelDoesNotApplyOrPersistExtensionState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings storage(directory.filePath(QStringLiteral("cancel.ini")), QSettings::IniFormat);
    storage.setValue(QStringLiteral("extension/value"), 7);
    storage.sync();

    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults().editor);
    auto* page = new SyntheticSettingsPage(QStringLiteral("synthetic/cancel"),
                                           QStringLiteral("Synthetic cancel page"), storage,
                                           QStringLiteral("extension/value"));
    dialog.addPage(page);
    page->setBufferedValue(42);

    auto* cancel = findButton(dialog, QDialogButtonBox::Cancel);
    QVERIFY(cancel != nullptr);
    cancel->click();

    QCOMPARE(page->applyCount, 0);
    QCOMPARE(storage.value(QStringLiteral("extension/value")).toInt(), 7);
}

void RefactorWave1ContractTest::resetBuffersExtensionDefaultsUntilSave() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings storage(directory.filePath(QStringLiteral("reset.ini")), QSettings::IniFormat);
    storage.setValue(QStringLiteral("extension/value"), 17);
    storage.sync();

    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults().editor);
    auto* page = new SyntheticSettingsPage(QStringLiteral("synthetic/reset"),
                                           QStringLiteral("Synthetic reset page"), storage,
                                           QStringLiteral("extension/value"), 3);
    dialog.addPage(page);

    auto* reset = findButton(dialog, QDialogButtonBox::NoButton, QStringLiteral("settingsReset"));
    QVERIFY(reset != nullptr);
    reset->click();

    QCOMPARE(page->resetCount, 1);
    QCOMPARE(page->bufferedValue(), 3);
    QCOMPARE(page->applyCount, 0);
    QCOMPARE(storage.value(QStringLiteral("extension/value")).toInt(), 17);

    auto* save = findButton(dialog, QDialogButtonBox::NoButton, QStringLiteral("primaryAction"));
    QVERIFY(save != nullptr);
    QVERIFY(save->isEnabled());
    save->click();

    QCOMPARE(page->applyCount, 1);
    QCOMPARE(storage.value(QStringLiteral("extension/value")).toInt(), 3);
}

void RefactorWave1ContractTest::themePreviewRollsBackOnCancel() {
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults(), theme);

    auto* themes = dialog.findChild<QListWidget*>(QStringLiteral("installedThemeList"));
    QVERIFY(themes != nullptr);
    QListWidgetItem* darkTheme = nullptr;
    for (int row = 0; row < themes->count(); ++row) {
        if (themes->item(row)->data(Qt::UserRole).toInt() ==
            static_cast<int>(ketplus::ThemeManager::Mode::Dark)) {
            darkTheme = themes->item(row);
            break;
        }
    }
    QVERIFY(darkTheme != nullptr);
    themes->setCurrentItem(darkTheme);
    QVERIFY(theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Light);

    auto* cancel = findButton(dialog, QDialogButtonBox::Cancel);
    QVERIFY(cancel != nullptr);
    cancel->click();

    QVERIFY(!theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Light);
}

void RefactorWave1ContractTest::saveCurrentlyAppliesEveryExtensionPage() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings storage(directory.filePath(QStringLiteral("multiple-pages.ini")),
                      QSettings::IniFormat);
    storage.setValue(QStringLiteral("dirty/value"), 1);
    storage.setValue(QStringLiteral("clean/value"), 2);
    storage.sync();

    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults().editor);
    auto* dirtyPage = new SyntheticSettingsPage(QStringLiteral("synthetic/dirty"),
                                                QStringLiteral("Synthetic dirty page"), storage,
                                                QStringLiteral("dirty/value"));
    auto* cleanPage = new SyntheticSettingsPage(QStringLiteral("synthetic/clean"),
                                                QStringLiteral("Synthetic clean page"), storage,
                                                QStringLiteral("clean/value"));
    dialog.addPage(dirtyPage);
    dialog.addPage(cleanPage);
    dirtyPage->setBufferedValue(9);

    auto* navigation = dialog.findChild<QListWidget*>(QStringLiteral("settingsNavigation"));
    QVERIFY(navigation != nullptr);

    QStringList extensionIds;
    for (int row = 0; row < navigation->count(); ++row) {
        const QString id = navigation->item(row)->data(Qt::UserRole).toString();
        if (id.startsWith(QStringLiteral("synthetic/"))) {
            extensionIds.append(id);
        }
    }
    QCOMPARE(extensionIds,
             (QStringList{QStringLiteral("synthetic/dirty"), QStringLiteral("synthetic/clean")}));

    auto* save = findButton(dialog, QDialogButtonBox::NoButton, QStringLiteral("primaryAction"));
    QVERIFY(save != nullptr);
    QVERIFY(save->isEnabled());
    save->click();

    QCOMPARE(dirtyPage->applyCount, 1);
    QCOMPARE(cleanPage->applyCount, 1);
    QCOMPARE(storage.value(QStringLiteral("dirty/value")).toInt(), 9);
    QCOMPARE(storage.value(QStringLiteral("clean/value")).toInt(), 2);
}

void RefactorWave1ContractTest::currentNativePaletteBaseline() {
    ketplus::ThemeManager theme(*qApp);

    theme.setMode(ketplus::ThemeManager::Mode::Light);
    const auto light = theme.palette();
    QVERIFY(!light.dark);
    QCOMPARE(light.pageBackground, QStringLiteral("#F7F5F5"));
    QCOMPARE(light.panelBackground, QStringLiteral("#FFFFFF"));
    QCOMPARE(light.textMain, QStringLiteral("#24262A"));
    QVERIFY(!light.focus.isEmpty());
    QVERIFY(!light.danger.isEmpty());

    theme.setMode(ketplus::ThemeManager::Mode::Dark);
    const auto dark = theme.palette();
    QVERIFY(dark.dark);
    QCOMPARE(dark.pageBackground, QStringLiteral("#1B1F24"));
    QCOMPARE(dark.panelBackground, QStringLiteral("#1D2228"));
    QCOMPARE(dark.accent, QStringLiteral("#5968DF"));
    QVERIFY(!dark.positive.isEmpty());
    QVERIFY(!dark.warning.isEmpty());
}

void RefactorWave1ContractTest::parserReadsObservedWorktreeFlags() {
    const QByteArray output = nulSeparated({
        "worktree /repo",
        "HEAD 11111111",
        "branch refs/heads/main",
        "",
        "worktree /repo-feature",
        "HEAD 22222222",
        "branch refs/heads/feature/public-contract",
        "locked integration review",
        "",
        "worktree /repo-detached",
        "HEAD 33333333",
        "detached",
        "prunable gitdir file points to non-existent location",
        "",
        "worktree /repo-bare",
        "bare",
        "unknown optional-field",
        "",
        "HEAD malformed-record-without-worktree",
        "",
    });

    const auto worktrees = ketplus::git_parser::parseWorktrees(output, QStringLiteral("/repo"));
    QCOMPARE(worktrees.size(), 4);

    QVERIFY(worktrees.at(0).current);
    QCOMPARE(worktrees.at(0).branch, QStringLiteral("main"));

    QCOMPARE(worktrees.at(1).branch, QStringLiteral("feature/public-contract"));
    QCOMPARE(worktrees.at(1).lockReason, QStringLiteral("integration review"));

    QVERIFY(worktrees.at(2).detached);
    QVERIFY(worktrees.at(2).prunable);
    QVERIFY(worktrees.at(2).branch.isEmpty());

    QVERIFY(worktrees.at(3).bare);
    QVERIFY(worktrees.at(3).head.isEmpty());
    QVERIFY(worktrees.at(3).branch.isEmpty());
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("SyntheticWave1Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("RefactorWave1ContractTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    RefactorWave1ContractTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "RefactorWave1ContractTest.moc"
