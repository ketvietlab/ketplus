#include "KetAppearance.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "KetStyle.h"
#include "KetSyntax.h"
#include "KetTokens.h"
#include "app/SettingsDialog.h"
#include "git/GitParser.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFontInfo>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolTip>
#include <QtTest>

#include <cmath>
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

QAbstractButton* findButton(ketplus::SettingsDialog& dialog,
                            const QDialogButtonBox::StandardButton role,
                            const QString& fallbackRole = {}) {
    auto* buttons = dialog.findChild<QWidget*>(QStringLiteral("settingsButtons"));
    if (buttons == nullptr) {
        return nullptr;
    }
    for (auto* button : buttons->findChildren<QAbstractButton*>()) {
        if ((role != QDialogButtonBox::NoButton &&
             button->property("standardButton").toInt() == int(role)) ||
            (!fallbackRole.isEmpty() && button->property("kvRole") == fallbackRole)) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class RefactorWave1ContractTest final : public QObject {
    Q_OBJECT

  private slots:
    void extensionChangesApplyAndPersistAtOnce();
    void restoreDefaultsAppliesAtOnce();
    void themeChoiceAppliesAndSurvivesClosing();
    void onlyChangedExtensionPagesApply();
    void currentNativePaletteBaseline();
    void themeFollowsKetAppearance();
    void themeInstallsTheKetStyle();
    void interfaceSizeUpdatesKitZoom();
    void parserReadsObservedWorktreeFlags();
};

// Every settings change applies as it is made (Jay, 2026-11-10): there is no draft, Save or
// Cancel, and closing the dialog keeps what was applied.
void RefactorWave1ContractTest::extensionChangesApplyAndPersistAtOnce() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings storage(directory.filePath(QStringLiteral("apply.ini")), QSettings::IniFormat);
    storage.setValue(QStringLiteral("extension/value"), 7);
    storage.sync();

    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults().editor);
    auto* page = new SyntheticSettingsPage(QStringLiteral("synthetic/apply"),
                                           QStringLiteral("Synthetic apply page"), storage,
                                           QStringLiteral("extension/value"));
    dialog.addPage(page);
    QCOMPARE(page->applyCount, 0);
    page->setBufferedValue(42);

    QCOMPARE(page->applyCount, 1);
    QCOMPARE(storage.value(QStringLiteral("extension/value")).toInt(), 42);
    QVERIFY(findButton(dialog, QDialogButtonBox::Cancel) == nullptr);
    QVERIFY(findButton(dialog, QDialogButtonBox::Save, QStringLiteral("primaryAction")) == nullptr);
}

void RefactorWave1ContractTest::restoreDefaultsAppliesAtOnce() {
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
    QCOMPARE(reset->text(), QStringLiteral("Restore defaults"));
    reset->click();

    QCOMPARE(page->resetCount, 1);
    QCOMPARE(page->bufferedValue(), 3);
    QCOMPARE(page->applyCount, 1);
    QCOMPARE(storage.value(QStringLiteral("extension/value")).toInt(), 3);
}

void RefactorWave1ContractTest::themeChoiceAppliesAndSurvivesClosing() {
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults(), theme);

    auto* darkTheme = dialog.findChild<QAbstractButton*>(QStringLiteral("darkThemeCard"));
    QVERIFY(darkTheme != nullptr);
    darkTheme->click();
    QVERIFY(theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);

    dialog.show();
    QTest::keyClick(&dialog, Qt::Key_Escape);
    QVERIFY(theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
}

void RefactorWave1ContractTest::onlyChangedExtensionPagesApply() {
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

    dirtyPage->setBufferedValue(9);
    QCOMPARE(dirtyPage->applyCount, 1);
    QCOMPARE(cleanPage->applyCount, 0);
    QCOMPARE(storage.value(QStringLiteral("dirty/value")).toInt(), 9);
    QCOMPARE(storage.value(QStringLiteral("clean/value")).toInt(), 2);
}

void RefactorWave1ContractTest::themeFollowsKetAppearance() {
    // The mode, a preview and its cancel go through ket::Appearance, which QML (KetTheme),
    // the kit's style and KétPlus Terminal read; a theme change there reaches the palette.
    ketplus::ThemeManager theme(*qApp);
    auto& appearance = ket::Appearance::instance();
    QSignalSpy changes(&theme, &ketplus::ThemeManager::themeChanged);

    theme.setMode(ketplus::ThemeManager::Mode::Dark);
    QCOMPARE(appearance.mode(), ket::Appearance::Mode::Dark);
    QVERIFY(theme.palette().dark);

    theme.previewMode(ketplus::ThemeManager::Mode::Light);
    QCOMPARE(appearance.mode(), ket::Appearance::Mode::Light);
    QVERIFY(!theme.palette().dark);
    theme.cancelPreview();
    QCOMPARE(appearance.mode(), ket::Appearance::Mode::Dark);
    QVERIFY(theme.palette().dark);

    theme.setMode(ketplus::ThemeManager::Mode::System);
    QCOMPARE(appearance.mode(), ket::Appearance::Mode::System);
    QCOMPARE(theme.palette().dark, appearance.dark());

    // Each switch applies once, not again when Appearance reports the same change.
    changes.clear();
    theme.setMode(ketplus::ThemeManager::Mode::Dark);
    QCOMPARE(changes.size(), 1);

    // A change made on ket::Appearance itself (KétPlus Terminal, or the system scheme while
    // following it) reaches the palette.
    changes.clear();
    appearance.setMode(ket::Appearance::Mode::Light);
    QCOMPARE(changes.size(), 1);
    QVERIFY(!theme.palette().dark);
    appearance.setMode(ket::Appearance::Mode::Dark);
    QCOMPARE(changes.size(), 2);
    QVERIFY(theme.palette().dark);
}

void RefactorWave1ContractTest::interfaceSizeUpdatesKitZoom() {
    ketplus::ThemeManager theme(*qApp);
    auto& appearance = ket::Appearance::instance();
    QSignalSpy changes(&appearance, &ket::Appearance::zoomChanged);
    // Include Lite's numeric sizes as well as the Pro shell's named steps. Repeat to
    // catch compounded scaling, and preserve the current zoom across theme changes.
    for (int pixels : {12, 16, 18, 14, 12, 16, 14}) {
        theme.setInterfaceFontSizePixels(pixels);
        QCOMPARE(appearance.zoom(), qreal(pixels) / 14);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(QToolTip::font().pixelSize(), qRound(13.0 * pixels / 14));
        theme.setMode(ketplus::ThemeManager::Mode::Dark);
        QCOMPARE(appearance.zoom(), qreal(pixels) / 14);
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        QCOMPARE(appearance.zoom(), qreal(pixels) / 14);
        theme.setInterfaceFontSizePixels(pixels);
        QCOMPARE(changes.size(), 1);
        changes.clear();
    }
}

void RefactorWave1ContractTest::themeInstallsTheKetStyle() {
    // KétPlus metrics (code at 14 px) and the kit's style: it keeps the application and
    // tooltip palettes on ket::Appearance, and draws menus, tooltips and tab bars, which the
    // stylesheet leaves to it.
    ketplus::ThemeManager theme(*qApp);
    QCOMPARE(ket::Profile::current().codePixels, 14.0);
    for (const auto mode :
         {ketplus::ThemeManager::Mode::Dark, ketplus::ThemeManager::Mode::Light}) {
        theme.setMode(mode);
        const QPalette expected = ket::Style::paletteFor(ket::Appearance::instance().theme());
        QCOMPARE(qApp->palette().color(QPalette::Window), expected.color(QPalette::Window));
        QCOMPARE(qApp->palette().color(QPalette::Highlight), expected.color(QPalette::Highlight));
        QCOMPARE(QToolTip::palette().color(QPalette::ToolTipBase),
                 expected.color(QPalette::ToolTipBase));
        // A theme switch rewrites the stylesheet, which drops per-class fonts.
        QCOMPARE(QToolTip::font().pixelSize(),
                 qRound(ket::Appearance::instance().scaled(
                     ket::tooltipRecipe(ket::Appearance::instance().theme()).fontPixels)));
    }
    // Tooltips keep the kit's font size from the start, not only after a theme change, and
    // after the interface size changes.
    const auto tooltipPixels = [] {
        return qRound(ket::Appearance::instance().scaled(
            ket::tooltipRecipe(ket::Appearance::instance().theme()).fontPixels));
    };
    // The label a tooltip shows: the stylesheet's base QWidget size must not reach it.
    const auto shownTooltipPixels = [] {
        QWidget anchor;
        anchor.resize(80, 40);
        anchor.show();
        QToolTip::showText(anchor.mapToGlobal(QPoint(10, 10)), QStringLiteral("Tip"), &anchor);
        int pixels = -1;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("QTipLabel") && widget->isVisible()) {
                widget->ensurePolished();
                pixels = QFontInfo(widget->font()).pixelSize();
            }
        }
        QToolTip::hideText();
        return pixels;
    };
    {
        // A clean start: the application has no stylesheet yet, as in main().
        qApp->setStyleSheet(QString());
        ketplus::ThemeManager fresh(*qApp);
        QCOMPARE(QToolTip::font().pixelSize(), tooltipPixels());
        QCOMPARE(shownTooltipPixels(), tooltipPixels());
        fresh.setInterfaceFontSizePixels(fresh.interfaceFontSizePixels() + 4);
        QCOMPARE(QToolTip::font().pixelSize(), tooltipPixels());
        QCOMPARE(shownTooltipPixels(), tooltipPixels());
        QCOMPARE(ketplus::tooltipFontPixels(), tooltipPixels());
    }
    const QString styleSheet = qApp->styleSheet();
    for (const char* selector : {"QMenu {", "QMenu::item", "QTabBar::tab"}) {
        QVERIFY2(!styleSheet.contains(QLatin1String(selector)), selector);
    }
    // The tooltip rule sets only the size; the kit's style still draws the panel.
    const qsizetype tooltipRule = styleSheet.indexOf(QLatin1String("QToolTip {"));
    QVERIFY(tooltipRule >= 0);
    const QString tooltipBody = styleSheet.mid(
        tooltipRule, styleSheet.indexOf(QLatin1Char('}'), tooltipRule) - tooltipRule);
    QVERIFY(!tooltipBody.contains(QLatin1String("background")));
    QVERIFY(!tooltipBody.contains(QLatin1String("border")));
    QVERIFY(!tooltipBody.contains(QLatin1String("padding")));
}

void RefactorWave1ContractTest::currentNativePaletteBaseline() {
    ketplus::ThemeManager theme(*qApp);
    const auto token = [](const char* name, const ket::Theme ketTheme) {
        return ket::Tokens::color(QString::fromLatin1(name), ketTheme).name().toUpper();
    };

    // Every role comes from a Két DS token (or the kit's syntax recipe), not a literal.
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    const auto light = theme.palette();
    QVERIFY(!light.dark);
    QCOMPARE(light.pageBackground, token("page-bg", ket::Theme::Light));
    QCOMPARE(light.panelBackground, token("panel-bg", ket::Theme::Light));
    QCOMPARE(light.textMain, token("text-main", ket::Theme::Light));
    QCOMPARE(light.accent, token("accent", ket::Theme::Light));
    QCOMPARE(light.info, token("info", ket::Theme::Light));
    QCOMPARE(light.actionPrimaryInk, token("action-primary-ink", ket::Theme::Light));
    QCOMPARE(light.syntaxKeyword, ket::syntaxRecipe(ket::Theme::Light).keyword.name().toUpper());
    QVERIFY(light.syntaxKeyword != light.syntaxType);
    QVERIFY(light.syntaxNumber != light.syntaxKeyword);
    QVERIFY(!light.focus.isEmpty());
    QVERIFY(!light.danger.isEmpty());

    theme.setMode(ketplus::ThemeManager::Mode::Dark);
    const auto dark = theme.palette();
    QVERIFY(dark.dark);
    QCOMPARE(dark.pageBackground, token("page-bg", ket::Theme::Dark));
    QCOMPARE(dark.panelBackground, token("panel-bg", ket::Theme::Dark));
    QCOMPARE(dark.accent, token("accent", ket::Theme::Dark));
    QCOMPARE(dark.textMain, token("text-main", ket::Theme::Dark));
    QCOMPARE(dark.info, token("info", ket::Theme::Dark));
    // Translucent dark borders are composited over the panel: opaque #RRGGBB.
    QCOMPARE(dark.border.size(), 7);
    QVERIFY(dark.border != dark.panelBackground);
    QVERIFY(!dark.positive.isEmpty());
    QVERIFY(!dark.warning.isEmpty());
    QVERIFY(!dark.syntaxString.isEmpty());
    QVERIFY(dark.syntaxKeyword != dark.syntaxNumber);

    // Code on the editor's panel reads at 4.5:1 (WCAG AA for 12 px text) in both themes.
    const auto luminance = [](const QColor& c) {
        const auto linear = [](qreal v) {
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
    };
    for (const auto& palette : {light, dark}) {
        const qreal panel = luminance(QColor(palette.panelBackground));
        for (const QString& role :
             {palette.syntaxKeyword, palette.syntaxString, palette.syntaxNumber, palette.syntaxType,
              palette.syntaxComment, palette.syntaxAttribute}) {
            const qreal a = luminance(QColor(role));
            const qreal ratio = (qMax(a, panel) + 0.05) / (qMin(a, panel) + 0.05);
            QVERIFY2(ratio >= 4.5, qPrintable(QStringLiteral("%1 on %2 is %3:1")
                                                  .arg(role, palette.panelBackground)
                                                  .arg(ratio)));
        }
    }
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
