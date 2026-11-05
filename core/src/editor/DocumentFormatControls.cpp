#include "editor/DocumentFormatControls.h"

#include "editor/EditorWidget.h"
#include "editor/SyntaxDefinition.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>

#include <utility>

namespace ketplus {

DocumentFormatControls::DocumentFormatControls(QWidget* window, Host host)
    : QObject(window), window_(window), host_(std::move(host)) {}

void DocumentFormatControls::addMenus(QMenu* menu) {
    languageMenu_ = menu->addMenu(QStringLiteral("Language"));
    lineEndingMenu_ = menu->addMenu(QStringLiteral("Line Endings"));
    encodingMenu_ = menu->addMenu(QStringLiteral("Encoding"));

    auto* languageGroup = new QActionGroup(this);
    const auto addLanguage = [this, languageGroup](const QString& label, const QString& name) {
        auto* action = languageMenu_->addAction(label);
        action->setCheckable(true);
        action->setData(name);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] { setSyntax(name); });
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
                [this, lineEnding] { setLineEnding(lineEnding); });
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

    for (QMenu* formatMenu : {languageMenu_, lineEndingMenu_, encodingMenu_}) {
        connect(formatMenu, &QMenu::aboutToShow, this, &DocumentFormatControls::syncMenus);
    }
}

void DocumentFormatControls::addStatusButtons(
    const std::function<QToolButton*(QMenu*, const QString&)>& makeButton) {
    encodingButton_ = makeButton(encodingMenu_, QStringLiteral("File encoding"));
    lineEndingButton_ = makeButton(lineEndingMenu_, QStringLiteral("Line endings"));
    languageButton_ = makeButton(languageMenu_, QStringLiteral("Language"));
}

void DocumentFormatControls::syncMenus() {
    if (languageMenu_ == nullptr) {
        return;
    }
    auto* editor = host_.document();
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

void DocumentFormatControls::updateIndicators() {
    if (encodingButton_ == nullptr) {
        return;
    }
    auto* editor = host_.document();
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
    syncMenus();
}

void DocumentFormatControls::setSyntax(const QString& syntaxName) {
    auto* editor = host_.document();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    editor->setSyntaxOverride(syntaxName);
    host_.restyle(editor);
    host_.syntaxChanged();
    updateIndicators();
}

void DocumentFormatControls::setLineEnding(const LineEnding lineEnding) {
    auto* editor = host_.document();
    if (editor != nullptr && !editor->isHibernated() && editor->lineEnding() != lineEnding) {
        editor->setLineEnding(lineEnding);
    }
    updateIndicators();
}

void DocumentFormatControls::reopenWithEncoding(const TextEncoding encoding) {
    auto* editor = host_.document();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    if (editor->document().isUntitled()) {
        QMessageBox::information(
            window_, QStringLiteral("Reopen with encoding"),
            QStringLiteral("Save the file before reopening it with another encoding."));
        return;
    }
    if (editor->document().isModified()) {
        const auto answer = QMessageBox::warning(
            window_, QStringLiteral("Unsaved changes"),
            QStringLiteral("Reopening %1 as %2 discards your unsaved changes.")
                .arg(editor->document().displayName(), textEncodingName(encoding)),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            return;
        }
    }

    const auto result = editor->reloadWithEncoding(encoding);
    if (!result.ok) {
        QMessageBox::critical(window_, QStringLiteral("Unable to reopen file"), result.error);
        return;
    }
    host_.restyle(editor);
    host_.reopened(editor);
}

void DocumentFormatControls::saveWithEncoding(const TextEncoding encoding) {
    auto* editor = host_.document();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    const TextEncoding previous = editor->document().textEncoding();
    editor->document().setTextEncoding(encoding);
    if (!host_.save(editor)) {
        editor->document().setTextEncoding(previous);
    }
    updateIndicators();
}

} // namespace ketplus
