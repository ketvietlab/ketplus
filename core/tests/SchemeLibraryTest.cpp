#include "app/theme/SchemeLibrary.h"
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/KetFonts.h"
#include "ui/theme/ThemeSelection.h"
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus;
class SchemeLibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { registerKetFonts(); }
    void detectsFormatsAndRoundTrips();
    void importsOnlyChosenVariants();
    void rejectsInvalidInputWithoutWriting();
    void cancelDoesNotImportAndAcceptWritesNewIdentity();
    void searchesBySourceAndProtectsBuiltins();
    void duplicatesAndDeletesAfterConfirmation();
    void persistsSelectionAndResetsRemovedPreferences();
    void lockedSelectionKeepsActiveScheme();
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
    ThemeSelectionStore store(root.path());
    auto state = store.selection();
    state.schemeId = added.scheme.id;
    state.darkSchemeId = added.scheme.id;
    state.followSystem = false;
    QVERIFY(store.commit(state));
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
    QCOMPARE(store.selection().schemeId, QStringLiteral("builtin-dark"));
    QCOMPARE(store.selection().darkSchemeId, QStringLiteral("builtin-dark"));
    QCOMPARE(catalog.list().size(), 2);
    QTRY_COMPARE(panel.selectedId(), QStringLiteral("builtin-dark"));
}
void SchemeLibraryTest::lockedSelectionKeepsActiveScheme() {
    QTemporaryDir root;
    ColorSchemeCatalog catalog(root.path());
    const auto added = catalog.duplicate(QStringLiteral("builtin-dark"), QStringLiteral("Keep me"));
    QVERIFY(added.success);
    ThemeSelectionStore store(root.path());
    auto state = store.selection();
    state.followSystem = false;
    state.schemeId = added.scheme.id;
    QVERIFY(store.commit(state));
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
    QCOMPARE(store.selection().schemeId, added.scheme.id);
    auto* error = panel.findChild<ket::Banner*>(QStringLiteral("schemeError"));
    QVERIFY(error->message().contains(QStringLiteral("busy")));
    QVERIFY(!error->isHidden());
}
QTEST_MAIN(SchemeLibraryTest)
#include "SchemeLibraryTest.moc"
