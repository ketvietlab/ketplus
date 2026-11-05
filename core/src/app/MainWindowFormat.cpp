#include "app/MainWindow.h"

#include "editor/EditorWidget.h"
#include "editor/SyntaxDefinition.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>

namespace ketplus {

EditorWidget* MainWindow::documentOwner() const {
    // The split view shares its source tab's document, file and encoding.
    auto* editor = activeEditor();
    return editor != nullptr && editor == splitEditor_ && splitSource_ != nullptr ? splitSource_
                                                                                  : editor;
}

void MainWindow::populateFormatMenus() {
    auto* languageGroup = new QActionGroup(this);
    const auto addLanguage = [this, languageGroup](const QString& label, const QString& name) {
        auto* action = languageMenu_->addAction(label);
        action->setCheckable(true);
        action->setData(name);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] { setEditorSyntax(name); });
    };
    addLanguage(QStringLiteral("Auto Detect"), QString());
    languageMenu_->addSeparator();
    for (const auto& choice : syntaxChoices()) {
        addLanguage(QString::fromLatin1(choice.displayName), QString::fromLatin1(choice.name));
    }

    auto* lineEndingGroup = new QActionGroup(this);
    for (const auto& [lineEnding, label] :
         {std::pair{LineEnding::Lf, QStringLiteral("LF — Unix and macOS")},
          std::pair{LineEnding::CrLf, QStringLiteral("CRLF — Windows")},
          std::pair{LineEnding::Cr, QStringLiteral("CR — Classic Mac OS")}}) {
        auto* action = lineEndingMenu_->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(lineEnding));
        lineEndingGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, lineEnding] { setDocumentLineEnding(lineEnding); });
    }

    auto* reopenMenu = encodingMenu_->addMenu(QStringLiteral("Reopen with Encoding"));
    saveEncodingMenu_ = encodingMenu_->addMenu(QStringLiteral("Save with Encoding"));
    auto* saveGroup = new QActionGroup(this);
    for (const TextEncoding encoding : supportedTextEncodings()) {
        auto* reopenAction = reopenMenu->addAction(textEncodingName(encoding));
        connect(reopenAction, &QAction::triggered, this,
                [this, encoding] { reopenWithEncoding(encoding); });
        auto* saveAction = saveEncodingMenu_->addAction(textEncodingName(encoding));
        saveAction->setCheckable(true);
        saveAction->setData(static_cast<int>(encoding));
        saveGroup->addAction(saveAction);
        connect(saveAction, &QAction::triggered, this,
                [this, encoding] { saveWithEncoding(encoding); });
    }

    for (QMenu* menu : {languageMenu_, lineEndingMenu_, encodingMenu_}) {
        connect(menu, &QMenu::aboutToShow, this, &MainWindow::syncFormatMenus);
    }
}

void MainWindow::syncFormatMenus() {
    auto* editor = documentOwner();
    const bool available = editor != nullptr && !editor->isHibernated();
    for (QMenu* menu : {languageMenu_, lineEndingMenu_, encodingMenu_}) {
        menu->setEnabled(available);
    }
    if (!available) {
        return;
    }

    for (QAction* action : languageMenu_->actions()) {
        if (action->isCheckable()) {
            action->setChecked(action->data().toString() == editor->syntaxOverride());
        }
    }
    for (QAction* action : lineEndingMenu_->actions()) {
        action->setChecked(action->data().toInt() == static_cast<int>(editor->lineEnding()));
    }
    for (QAction* action : saveEncodingMenu_->actions()) {
        action->setChecked(action->data().toInt() ==
                           static_cast<int>(editor->document().textEncoding()));
    }
}

void MainWindow::updateFormatIndicators() {
    if (encodingButton_ == nullptr) {
        return;
    }
    auto* editor = documentOwner();
    const bool visible = editor != nullptr && !editor->isHibernated();
    for (QToolButton* button : {encodingButton_, lineEndingButton_, languageButton_}) {
        button->setVisible(visible);
    }
    if (!visible) {
        return;
    }

    encodingButton_->setText(textEncodingName(editor->document().textEncoding()));
    lineEndingButton_->setText(lineEndingName(editor->lineEnding()));
    languageButton_->setText(syntaxDisplayName(editor->syntaxName()));
    languageButton_->setToolTip(editor->syntaxOverride().isEmpty()
                                    ? QStringLiteral("Language detected from the file name")
                                    : QStringLiteral("Language chosen manually"));
    syncFormatMenus();
}

void MainWindow::setEditorSyntax(const QString& syntaxName) {
    auto* editor = documentOwner();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    editor->setSyntaxOverride(syntaxName);
    editor->applyTheme(theme_.palette());
    if (editor == splitSource_ && splitEditor_ != nullptr) {
        splitEditor_->shareDocumentWith(*editor);
        splitEditor_->applyTheme(theme_.palette());
    }
    updateMarkdownPreview();
    updateFormatIndicators();
}

void MainWindow::setDocumentLineEnding(const LineEnding lineEnding) {
    auto* editor = documentOwner();
    if (editor == nullptr || editor->isHibernated() || editor->lineEnding() == lineEnding) {
        updateFormatIndicators();
        return;
    }
    editor->setLineEnding(lineEnding);
    updateFormatIndicators();
}

void MainWindow::reopenWithEncoding(const TextEncoding encoding) {
    auto* editor = documentOwner();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    if (editor->document().isUntitled()) {
        QMessageBox::information(this, QStringLiteral("Reopen with encoding"),
                                 QStringLiteral("Save the file before reopening it with another "
                                                "encoding."));
        return;
    }
    if (editor->document().isModified()) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Unsaved changes"),
            QStringLiteral("Reopening %1 as %2 discards your unsaved changes.")
                .arg(editor->document().displayName(), textEncodingName(encoding)),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            return;
        }
    }

    const auto result = editor->reloadWithEncoding(encoding);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to reopen file"), result.error);
        return;
    }
    editor->applyTheme(theme_.palette());
    if (editor == splitSource_ && splitEditor_ != nullptr) {
        splitEditor_->shareDocumentWith(*editor);
        splitEditor_->applyTheme(theme_.palette());
    }
    updateTabTitle(editor);
    updateDocumentState();
    updateMarkdownPreview();
}

void MainWindow::saveWithEncoding(const TextEncoding encoding) {
    auto* editor = documentOwner();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    const TextEncoding previous = editor->document().textEncoding();
    editor->document().setTextEncoding(encoding);
    if (!saveEditor(editor, false)) {
        editor->document().setTextEncoding(previous);
    }
    updateFormatIndicators();
}

} // namespace ketplus
