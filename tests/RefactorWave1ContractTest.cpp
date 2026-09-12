#include "app/SettingsDialog.h"
#include "git/GitParser.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QtTest>

#include <initializer_list>

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
    SyntheticSettingsPage()
        : SettingsPage(QStringLiteral("synthetic/page"),
                       QStringLiteral("Synthetic public extension")) {}

    [[nodiscard]] bool hasChanges() const override { return dirty; }

    void resetToDefaults() override {
        ++resetCount;
        dirty = true;
        emit changed();
    }

    void apply() override {
        ++applyCount;
        dirty = false;
    }

    void markDirty() {
        dirty = true;
        emit changed();
    }

    bool dirty{false};
    int resetCount{0};
    int applyCount{0};
};

} // namespace

class RefactorWave1ContractTest final : public QObject {
    Q_OBJECT

  private slots:
    void settingsExtensionPageUsesStableIdAndSharedActions();
    void themePaletteExposesKdsReferenceRoles();
    void syntheticWorktreeFixtureCapturesOperationGuards();
};

void RefactorWave1ContractTest::settingsExtensionPageUsesStableIdAndSharedActions() {
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults().editor);
    auto* page = new SyntheticSettingsPage;
    dialog.addPage(page);

    auto* navigation = dialog.findChild<QListWidget*>(QStringLiteral("settingsNavigation"));
    QVERIFY(navigation != nullptr);

    bool foundStableId = false;
    for (int row = 0; row < navigation->count(); ++row) {
        if (navigation->item(row)->data(Qt::UserRole).toString() == page->id()) {
            foundStableId = true;
            break;
        }
    }
    QVERIFY(foundStableId);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(buttons != nullptr);

    auto* save = buttons->button(QDialogButtonBox::Save);
    if (save == nullptr) {
        for (auto* button : buttons->findChildren<QPushButton*>()) {
            if (button->property("kvRole") == QStringLiteral("primaryAction")) {
                save = button;
                break;
            }
        }
    }
    QVERIFY(save != nullptr);
    QVERIFY(!save->isEnabled());

    page->markDirty();
    QVERIFY(save->isEnabled());

    QPushButton* reset = nullptr;
    for (auto* button : buttons->findChildren<QPushButton*>()) {
        if (button->property("kvRole") == QStringLiteral("settingsReset")) {
            reset = button;
            break;
        }
    }
    QVERIFY(reset != nullptr);
    reset->click();
    QCOMPARE(page->resetCount, 1);
    QVERIFY(save->isEnabled());

    save->click();
    QCOMPARE(page->applyCount, 1);
}

void RefactorWave1ContractTest::themePaletteExposesKdsReferenceRoles() {
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

void RefactorWave1ContractTest::syntheticWorktreeFixtureCapturesOperationGuards() {
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
        "HEAD 44444444",
        "bare",
        "",
    });

    const auto worktrees =
        ketplus::git_parser::parseWorktrees(output, QStringLiteral("/repo"));
    QCOMPARE(worktrees.size(), 4);

    QVERIFY(worktrees.at(0).current);
    QCOMPARE(worktrees.at(0).branch, QStringLiteral("main"));

    QCOMPARE(worktrees.at(1).branch, QStringLiteral("feature/public-contract"));
    QCOMPARE(worktrees.at(1).lockReason, QStringLiteral("integration review"));

    QVERIFY(worktrees.at(2).detached);
    QVERIFY(worktrees.at(2).prunable);
    QVERIFY(worktrees.at(2).branch.isEmpty());

    QVERIFY(worktrees.at(3).bare);
}

QTEST_MAIN(RefactorWave1ContractTest)
#include "RefactorWave1ContractTest.moc"
