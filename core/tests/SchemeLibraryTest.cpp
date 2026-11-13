#include "app/theme/SchemeLibrary.h"
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetCommandPalette.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "app/SettingsDialog.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "app/theme/SchemeQuickPicker.h"
#include "ui/KetFonts.h"
#include "ui/Theme.h"
#include "ui/theme/ThemeSelection.h"
#include <QDir>
#include <QFile>
#include <QLayout>
#include <QLockFile>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QWindow>
#include <QtTest>

using namespace ketplus;
class SchemeLibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        registerKetFonts();
    }
    void detectsFormatsAndRoundTrips();
    void importsOnlyChosenVariants();
    void rejectsInvalidInputWithoutWriting();
    void cancelDoesNotImportAndAcceptWritesNewIdentity();
    void searchesBySourceAndProtectsBuiltins();
    void duplicatesAndDeletesAfterConfirmation();
    void persistsSelectionAndResetsRemovedPreferences();
    void lockedSelectionKeepsActiveScheme();
    void appliesSelectedSchemeAndPreferredVariant();
    void previewsImportAndRestoresOnCancel();
    void keyboardPickerPreviewsCommitsAndRestores();
    void appearanceLayoutAndReviewCaptures();
};
void SchemeLibraryTest::detectsFormatsAndRoundTrips() {
    QTemporaryDir root;
    SchemeLibrary library(root.path());
    for (const auto& relative :
         {QStringLiteral("vscode/light.json"), QStringLiteral("jetbrains/dark.icls"),
          QStringLiteral("jetbrains/sample.theme.json")}) {
        const auto batch = SchemeLibrary::inspect(QStringLiteral(SCHEME_FIXTURES "/") + relative);
        QVERIFY2(batch.ok(), qPrintable(SchemeLibrary::diagnosticText(batch.diagnostics)));
        QCOMPARE(batch.candidates.size(), 1);
        const auto added = library.importCandidate(batch.candidates.first());
        QVERIFY(added.success);
        QCOMPARE(SchemeLibrary::sourceLabel(added.scheme), relative.startsWith(u"vscode")
                                                               ? QStringLiteral("VS Code")
                                                               : QStringLiteral("JetBrains"));
    }
    QList<SchemeDiagnostic> diagnostics;
    const auto path = root.filePath(QStringLiteral("export.ketscheme.json"));
    QVERIFY(library.exportScheme(QStringLiteral("builtin-dark"), path, &diagnostics));
    const auto native = SchemeLibrary::inspect(path);
    QVERIFY(native.ok());
    const auto added = library.importCandidate(native.candidates.first());
    QVERIFY(added.success);
    QVERIFY(!added.scheme.id.startsWith(u"builtin-"));
    QCOMPARE(added.scheme.editor, native.candidates.first().scheme.editor);
    QVERIFY(!library.exportScheme(added.scheme.id, root.path(), &diagnostics));
    QVERIFY(!diagnostics.isEmpty());
}
void SchemeLibraryTest::importsOnlyChosenVariants() {
    QTemporaryDir root;
    SchemeLibraryPanel panel(root.path());
    panel.resize(900, 700);
    panel.show();
    panel.inspectFile(QStringLiteral(SCHEME_FIXTURES "/scheme-ui/variants.vsix"));
    ket::Dialog* variants = nullptr;
    QTRY_VERIFY((variants = panel.findChild<ket::Dialog*>(QStringLiteral("schemeVariants"))));
    const auto choices = variants->findChildren<ket::Checkbox*>();
    QCOMPARE(choices.size(), 2);
    choices.last()->setChecked(false);
    for (auto* button : variants->findChildren<ket::Button*>())
        if (button->text() == QStringLiteral("Review import"))
            QTest::mouseClick(button, Qt::LeftButton);
    ket::Dialog* report = nullptr;
    QTRY_VERIFY((report = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
    const auto text =
        report->findChild<ket::TextArea*>(QStringLiteral("schemeImportDetails"))->text();
    QVERIFY(text.contains(QStringLiteral("Night variant")));
    QVERIFY(!text.contains(QStringLiteral("Paper variant")));
    QTest::mouseClick(report->findChild<ket::Button*>(QStringLiteral("acceptSchemeImport")),
                      Qt::LeftButton);
    const auto schemes = ColorSchemeCatalog(root.path()).list();
    QCOMPARE(schemes.size(), 3);
    QCOMPARE(schemes.last().name, QStringLiteral("Night variant"));
}
void SchemeLibraryTest::rejectsInvalidInputWithoutWriting() {
    QTemporaryDir root;
    SchemeLibrary library(root.filePath(QStringLiteral("library")));
    for (const auto& suffix :
         {QStringLiteral("json"), QStringLiteral("icls"), QStringLiteral("vsix"),
          QStringLiteral("jar"), QStringLiteral("txt")}) {
        QFile bad(root.filePath(QStringLiteral("bad.") + suffix));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("invalid input");
        bad.close();
        const auto batch = SchemeLibrary::inspect(bad.fileName());
        QVERIFY(!batch.ok());
        QVERIFY(!batch.diagnostics.isEmpty());
    }
    QCOMPARE(library.catalog().list().size(), 2);
    QVERIFY(!QDir(library.catalog().directory()).exists());
}
void SchemeLibraryTest::cancelDoesNotImportAndAcceptWritesNewIdentity() {
    QTemporaryDir root;
    SchemeLibraryPanel panel(root.path());
    panel.resize(900, 700);
    panel.show();
    const auto path = QStringLiteral(SCHEME_FIXTURES "/vscode/light.json");
    QSignalSpy changed(&panel, &SchemeLibraryPanel::catalogChanged);
    QSignalSpy finished(&panel, &SchemeLibraryPanel::previewFinished);
    panel.inspectFile(path);
    auto* dialog = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"));
    QTRY_VERIFY((dialog = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
    QVERIFY(dialog->findChild<ket::TextArea*>(QStringLiteral("schemeImportDetails"))
                ->text()
                .contains("Mapped"));
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
    QTest::keyClick(dialog, Qt::Key_Escape);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel.inspectFile(path);
    QTRY_VERIFY((dialog = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
    auto* accept = dialog->findChild<ket::Button*>(QStringLiteral("acceptSchemeImport"));
    QVERIFY(accept);
    QTest::mouseClick(accept, Qt::LeftButton);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 3);
    QVERIFY(!panel.selectedId().startsWith(u"builtin-"));
}
void SchemeLibraryTest::searchesBySourceAndProtectsBuiltins() {
    QTemporaryDir root;
    SchemeLibrary library(root.path());
    const auto batch =
        SchemeLibrary::inspect(QStringLiteral(SCHEME_FIXTURES "/jetbrains/dark.icls"));
    QVERIFY(batch.ok());
    QVERIFY(library.importCandidate(batch.candidates.first()).success);
    SchemeLibraryPanel panel(root.path());
    panel.resize(900, 700);
    panel.show();
    QVERIFY(!panel.findChild<ket::Button*>(QStringLiteral("deleteScheme"))->isEnabled());
    auto* search = panel.findChild<ket::TextField*>(QStringLiteral("schemeSearch"));
    search->setText(QStringLiteral("JetBrains"));
    QCOMPARE(panel.findChildren<ket::Choice*>().size(), 1);
    search->setText(QStringLiteral("nonexistent-name"));
    QCOMPARE(panel.findChildren<ket::Choice*>().size(), 0);
    panel.inspectFile(root.filePath(QStringLiteral("missing.json")));
    auto* error = panel.findChild<ket::Banner*>(QStringLiteral("schemeError"));
    QTRY_COMPARE(error->title(), QStringLiteral("Color scheme error"));
    QVERIFY(!error->isHidden());
}
void SchemeLibraryTest::duplicatesAndDeletesAfterConfirmation() {
    QTemporaryDir root;
    SchemeLibraryPanel panel(root.path());
    panel.resize(900, 700);
    panel.show();
    QTest::mouseClick(panel.findChild<ket::Button*>(QStringLiteral("duplicateScheme")),
                      Qt::LeftButton);
    auto* dialog = panel.findChild<ket::Dialog*>(QStringLiteral("duplicateSchemeDialog"));
    QVERIFY(dialog);
    auto* name = dialog->findChild<ket::TextField*>(QStringLiteral("duplicateSchemeName"));
    name->setText(QStringLiteral("My scheme"));
    for (auto* button : dialog->findChildren<ket::Button*>())
        if (button->text() == QStringLiteral("Duplicate"))
            QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 3);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto duplicated = panel.selectedId();
    QSignalSpy removed(&panel, &SchemeLibraryPanel::schemeRemoved);
    QTest::mouseClick(panel.findChild<ket::Button*>(QStringLiteral("deleteScheme")),
                      Qt::LeftButton);
    auto* confirm = panel.findChild<ket::Button*>(QStringLiteral("confirmDeleteScheme"));
    QVERIFY(confirm);
    QTest::mouseClick(confirm, Qt::LeftButton);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.first().first().toString(), duplicated);
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
    QVERIFY(panel.selectedId().startsWith(u"builtin-"));
}
void SchemeLibraryTest::persistsSelectionAndResetsRemovedPreferences() {
    QTemporaryDir root;
    const auto directory = root.filePath(QStringLiteral("schemes"));
    ColorSchemeCatalog catalog(directory);
    const auto added =
        catalog.duplicate(QStringLiteral("builtin-dark"), QStringLiteral("Custom night"));
    QVERIFY(added.success);
    ThemeManager store(*qApp, nullptr, root.path());
    auto state = store.schemeSelection();
    state.schemeId = added.scheme.id;
    state.darkSchemeId = added.scheme.id;
    state.followSystem = false;
    QVERIFY(store.setSchemeSelection(state));
    SchemeLibraryPanel panel(directory, nullptr, &store);
    panel.resize(900, 900);
    panel.show();
    QCOMPARE(panel.selectedId(), added.scheme.id);
    auto* dark = panel.findChild<ket::Select*>(QStringLiteral("preferredDarkScheme"));
    QVERIFY(dark);
    QCOMPARE(dark->currentData().toString(), added.scheme.id);
    auto* remove = panel.findChild<ket::Button*>(QStringLiteral("deleteScheme"));
    QVERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    auto* confirm = panel.findChild<ket::Button*>(QStringLiteral("confirmDeleteScheme"));
    QVERIFY(confirm);
    QTest::mouseClick(confirm, Qt::LeftButton);
    QCOMPARE(store.schemeSelection().schemeId, QStringLiteral("builtin-dark"));
    QCOMPARE(store.schemeSelection().darkSchemeId, QStringLiteral("builtin-dark"));
    QCOMPARE(catalog.list().size(), 2);
    QTRY_COMPARE(panel.selectedId(), QStringLiteral("builtin-dark"));
}
void SchemeLibraryTest::lockedSelectionKeepsActiveScheme() {
    QTemporaryDir root;
    ColorSchemeCatalog catalog(root.path());
    const auto added = catalog.duplicate(QStringLiteral("builtin-dark"), QStringLiteral("Keep me"));
    QVERIFY(added.success);
    ThemeManager store(*qApp, nullptr, root.path());
    auto state = store.schemeSelection();
    state.followSystem = false;
    state.schemeId = added.scheme.id;
    QVERIFY(store.setSchemeSelection(state));
    QLockFile lock(root.filePath(QStringLiteral(".selection.lock")));
    QVERIFY(lock.tryLock(0));
    SchemeLibraryPanel panel(root.path(), nullptr, &store);
    panel.resize(900, 900);
    panel.show();
    QTest::mouseClick(panel.findChild<ket::Button*>(QStringLiteral("deleteScheme")),
                      Qt::LeftButton);
    auto* confirm = panel.findChild<ket::Button*>(QStringLiteral("confirmDeleteScheme"));
    QVERIFY(confirm);
    QTest::mouseClick(confirm, Qt::LeftButton);
    QCOMPARE(catalog.list().size(), 3);
    QCOMPARE(store.schemeSelection().schemeId, added.scheme.id);
    auto* error = panel.findChild<ket::Banner*>(QStringLiteral("schemeError"));
    QVERIFY(error->message().contains(QStringLiteral("busy")));
    QVERIFY(!error->isHidden());
}
void SchemeLibraryTest::appliesSelectedSchemeAndPreferredVariant() {
    QTemporaryDir root;
    SchemeLibrary library(root.path());
    const auto batch =
        SchemeLibrary::inspect(QStringLiteral(SCHEME_FIXTURES "/jetbrains/dark.icls"));
    QVERIFY(batch.ok());
    const auto custom = library.importCandidate(batch.candidates.first());
    QVERIFY(custom.success);
    ThemeManager theme(*qApp, nullptr, root.path());
    auto initial = theme.schemeSelection();
    initial.followSystem = false;
    initial.schemeId = QStringLiteral("builtin-light");
    QVERIFY(theme.setSchemeSelection(initial));
    SchemeLibraryPanel panel(root.path(), nullptr, &theme);
    panel.resize(900, 700);
    panel.show();
    ket::Choice* row = nullptr;
    for (auto* candidate : panel.findChildren<ket::Choice*>())
        if (candidate->property("schemeId").toString() == custom.scheme.id)
            row = candidate;
    QVERIFY(row);
    QTest::mouseClick(row, Qt::LeftButton);
    QCOMPARE(theme.schemeSelection().schemeId, custom.scheme.id);
    QTRY_COMPARE(theme.colorScheme().id, custom.scheme.id);
    QCOMPARE(theme.colorScheme().editor.value("bg"), QColor("#101820"));
    auto* dark = panel.findChild<ket::Select*>(QStringLiteral("preferredDarkScheme"));
    QVERIFY(dark);
    const int index = dark->findData(custom.scheme.id);
    QVERIFY(index >= 0);
    dark->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(dark, "activated", Q_ARG(int, index)));
    QCOMPARE(theme.schemeSelection().darkSchemeId, custom.scheme.id);
}
void SchemeLibraryTest::previewsImportAndRestoresOnCancel() {
    QTemporaryDir root;
    ThemeManager theme(*qApp, nullptr, root.path());
    auto initial = theme.schemeSelection();
    initial.followSystem = false;
    initial.schemeId = QStringLiteral("builtin-light");
    QVERIFY(theme.setSchemeSelection(initial));
    SchemeLibraryPanel panel(root.path(), nullptr, &theme);
    panel.resize(900, 700);
    panel.show();
    panel.inspectFile(QStringLiteral(SCHEME_FIXTURES "/jetbrains/dark.icls"));
    ket::Dialog* dialog = nullptr;
    QTRY_VERIFY((dialog = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
    auto* preview = dialog->findChild<ket::Button*>(QStringLiteral("previewImportedScheme"));
    QVERIFY(preview);
    QTest::mouseClick(preview, Qt::LeftButton);
    QTRY_COMPARE(theme.colorScheme().editor.value("bg"), QColor("#101820"));
    QVERIFY(theme.hasSchemePreview());
    QCOMPARE(theme.schemeSelection(), initial);
    QVERIFY(!dialog->isVisible());
    auto* notice = panel.findChild<ket::Banner*>(QStringLiteral("schemePreviewBanner"));
    QVERIFY(notice && notice->isVisible());
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
    QTest::mouseClick(notice->action(), Qt::LeftButton);
    QVERIFY(dialog->isVisible());
    QTest::keyClick(dialog, Qt::Key_Escape);
    QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("builtin-light"));
    QVERIFY(!theme.hasSchemePreview());
    QCOMPARE(theme.schemeSelection(), initial);
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel.inspectFile(QStringLiteral(SCHEME_FIXTURES "/jetbrains/dark.icls"));
    QTRY_VERIFY((dialog = panel.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
    QTest::mouseClick(dialog->findChild<ket::Button*>(QStringLiteral("previewImportedScheme")),
                      Qt::LeftButton);
    QTRY_VERIFY(theme.hasSchemePreview());
    notice = panel.findChild<ket::Banner*>(QStringLiteral("schemePreviewBanner"));
    QVERIFY(notice && notice->isVisible());
    QTest::keyClick(notice->action(), Qt::Key_Escape);
    QTRY_VERIFY(!theme.hasSchemePreview());
    QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("builtin-light"));
    QCOMPARE(ColorSchemeCatalog(root.path()).list().size(), 2);
}
void SchemeLibraryTest::keyboardPickerPreviewsCommitsAndRestores() {
    QTemporaryDir root;
    ThemeManager theme(*qApp, nullptr, root.path());
    auto initial = theme.schemeSelection();
    initial.followSystem = false;
    initial.schemeId = QStringLiteral("builtin-light");
    QVERIFY(theme.setSchemeSelection(initial));
    QWidget window;
    window.resize(900, 700);
    window.show();
    QPointer<SchemeQuickPicker> picker = new SchemeQuickPicker(theme, &window, root.path());
    picker->open();
    QTest::keyClick(picker->palette()->searchField(), Qt::Key_Down);
    QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("builtin-dark"));
    QCOMPARE(theme.schemeSelection(), initial);
    QTest::keyClick(picker->palette()->searchField(), Qt::Key_Escape);
    QTRY_VERIFY(picker.isNull());
    QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("builtin-light"));
    QCOMPARE(theme.schemeSelection(), initial);
    picker = new SchemeQuickPicker(theme, &window, root.path());
    picker->open();
    QTest::keyClick(picker->palette()->searchField(), Qt::Key_Down);
    QTest::keyClick(picker->palette()->searchField(), Qt::Key_Return);
    QTRY_VERIFY(picker.isNull());
    QCOMPARE(theme.schemeSelection().schemeId, QStringLiteral("builtin-dark"));
    QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("builtin-dark"));
    QVERIFY(!theme.hasSchemePreview());
    QVERIFY(theme.setSchemeSelection(initial));
    picker = new SchemeQuickPicker(theme, &window, root.path());
    picker->open();
    QTest::keyClick(picker->palette()->searchField(), Qt::Key_Down);
    QVERIFY(theme.hasSchemePreview());
    ThemeSelectionStore otherProcess(root.path());
    auto external = otherProcess.selection();
    external.followSystem = false;
    external.schemeId = QStringLiteral("builtin-dark");
    QVERIFY(otherProcess.commit(external));
    QTRY_VERIFY(picker.isNull());
    QCOMPARE(theme.schemeSelection(), external);
    QVERIFY(!theme.hasSchemePreview());
}
void SchemeLibraryTest::appearanceLayoutAndReviewCaptures() {
    const bool native = qEnvironmentVariableIsSet("KETPLUS_SCHEME_CAPTURE_NATIVE");
    QScreen* captureScreen = nullptr;
    if (native) {
        for (auto* screen : qApp->screens())
            if ((screen->name() + screen->manufacturer() + screen->model())
                    .contains("DELL", Qt::CaseInsensitive))
                captureScreen = screen;
        QVERIFY2(captureScreen,
                 "Native review requires the secondary DELL display; no window was opened");
        QVERIFY2(captureScreen != qApp->primaryScreen(),
                 "The DELL capture display must be secondary");
        qInfo() << "Native scheme review screen:" << captureScreen->name()
                << captureScreen->geometry();
    }
    const auto output = qEnvironmentVariable("KETPLUS_SCHEME_CAPTURE_DIR");
    if (!output.isEmpty())
        QVERIFY(QDir().mkpath(output));
    const auto previousName = qApp->applicationName();
    qApp->setApplicationName(QStringLiteral("scheme-review-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto cleanup = qScopeGuard([profile, previousName] {
        qApp->setApplicationName(previousName);
        QDir(profile).removeRecursively();
    });
    {
        SchemeLibrary library;
        for (const auto& fixture :
             {QStringLiteral("vscode/light.json"), QStringLiteral("jetbrains/dark.icls")}) {
            const auto batch =
                SchemeLibrary::inspect(QStringLiteral(SCHEME_FIXTURES "/") + fixture);
            QVERIFY(batch.ok());
            QVERIFY(library.importCandidate(batch.candidates.first()).success);
        }
        ThemeManager theme(*qApp);
        SettingsDialog dialog(AppearanceSettings::defaults(), theme);
        dialog.resize(1100, 900);
        if (native) {
            dialog.setScreen(captureScreen);
            dialog.move(captureScreen->availableGeometry().topLeft() + QPoint(32, 32));
        }
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        if (native)
            QCOMPARE(dialog.windowHandle()->screen(), captureScreen);
        auto* panel = dialog.findChild<SchemeLibraryPanel*>();
        QVERIFY(panel);
        auto preferred = theme.schemeSelection();
        for (const auto& scheme : library.catalog().list())
            if (!scheme.id.startsWith(u"builtin-") && scheme.kind == SchemeKind::Dark)
                preferred.darkSchemeId = scheme.id;
        QVERIFY(theme.setSchemeSelection(preferred));
        auto* reset = dialog.findChild<ket::Button*>(QStringLiteral("settingsResetButton"));
        QVERIFY(reset);
        QTest::mouseClick(reset, Qt::LeftButton);
        QCOMPARE(theme.schemeSelection(), ThemeSelection{});
        auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("appearanceSettingsScroll"));
        QVERIFY(scroll);
        for (auto mode : {ThemeManager::Mode::Light, ThemeManager::Mode::Dark}) {
            theme.setMode(mode);
            for (int zoom : {90, 100, 110, 125}) {
                theme.setInterfaceScale(14, zoom);
                for (const auto& size : {QSize(760, 650), QSize(1100, 900)}) {
                    dialog.resize(size);
                    QCoreApplication::processEvents();
                    if (scroll->horizontalScrollBar()->maximum() != 0) {
                        qInfo() << "Layout cell" << int(mode) << zoom << size << "content minimum"
                                << scroll->widget()->minimumSizeHint() << "library minimum"
                                << panel->minimumSizeHint();
                        const auto* cardLayout = panel->parentWidget()->layout();
                        for (int i = 0; i < cardLayout->count(); ++i)
                            qInfo() << "Theme section" << i << cardLayout->itemAt(i)->minimumSize();
                    }
                    QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                    auto* search =
                        panel->findChild<ket::TextField*>(QStringLiteral("schemeSearch"));
                    auto* import = panel->findChild<ket::Button*>(QStringLiteral("importScheme"));
                    QVERIFY(search && import);
                    QVERIFY(!search->geometry().intersects(import->geometry()));
                }
            }
            theme.setInterfaceScale(14, 100);
            QCoreApplication::processEvents();
            QTRY_VERIFY(panel->findChild<ket::Select*>(QStringLiteral("preferredLightScheme"))
                            ->isVisibleTo(&dialog));
            QTRY_VERIFY(panel->findChild<ket::Select*>(QStringLiteral("preferredDarkScheme"))
                            ->isVisibleTo(&dialog));
            if (!output.isEmpty())
                QVERIFY(dialog.grab().save(QDir(output).filePath(
                    mode == ThemeManager::Mode::Light ? QStringLiteral("appearance-light.png")
                                                      : QStringLiteral("appearance-dark.png"))));
        }
        panel->inspectFile(QStringLiteral(SCHEME_FIXTURES "/jetbrains/dark.icls"));
        ket::Dialog* report = nullptr;
        QTRY_VERIFY(
            (report = dialog.findChild<ket::Dialog*>(QStringLiteral("schemeImportReport"))));
        QVERIFY(report->isVisible());
        QCoreApplication::processEvents();
        if (!output.isEmpty())
            QVERIFY(dialog.grab().save(QDir(output).filePath(QStringLiteral("import-report.png"))));
        QTest::keyClick(report, Qt::Key_Escape);
        QCOMPARE(library.catalog().list().size(), 4);
        dialog.close();
    }
}
QTEST_MAIN(SchemeLibraryTest)
#include "SchemeLibraryTest.moc"
