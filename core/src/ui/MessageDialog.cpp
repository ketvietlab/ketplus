#include "ui/MessageDialog.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetTextField.h"
#include <QEventLoop>
#include <QIntValidator>
#include <QLabel>
#include <QPointer>
#include <QShortcut>
#include <QVBoxLayout>
namespace ketplus::message_dialog {
QMessageBox::StandardButton choose(QWidget* parent, const QString& title, const QString& text,
                                   const QList<Choice>& choices,
                                   QMessageBox::StandardButton defaultButton) {
    Q_ASSERT(parent);
    if (!parent)
        return QMessageBox::Cancel;
    QPointer<ket::Dialog> dialog = new ket::Dialog(title, parent);
    dialog->setObjectName(QStringLiteral("messageDialog"));
    auto* body = new QLabel(text, dialog);
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dialog->setBody(body);
    QEventLoop loop;
    auto result = QMessageBox::Cancel;
    for (const auto& choice : choices) {
        auto* button = dialog->addButton(
            choice.text, choice.destructive              ? ket::ButtonVariant::Danger
                         : choice.value == defaultButton ? ket::ButtonVariant::Primary
                                                         : ket::ButtonVariant::Secondary);
        button->setProperty("standardButton", int(choice.value));
        QObject::connect(button, &QAbstractButton::clicked, &loop, [&, value = choice.value] {
            result = value;
            if (dialog)
                dialog->accept();
        });
        if (choice.value == defaultButton) {
            auto* enter = new QShortcut(QKeySequence(Qt::Key_Return), dialog);
            enter->setContext(Qt::WidgetWithChildrenShortcut);
            QObject::connect(enter, &QShortcut::activated, button, &QAbstractButton::click);
        }
    }
    QObject::connect(dialog, &ket::Dialog::finished, &loop, &QEventLoop::quit);
    QObject::connect(dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
    dialog->open();
    loop.exec();
    delete dialog.data();
    return result;
}
static QMessageBox::StandardButton message(QWidget* parent, const QString& title,
                                           const QString& text,
                                           QMessageBox::StandardButtons buttons,
                                           QMessageBox::StandardButton defaultButton) {
    const QList<Choice> available = {{QMessageBox::Save, QObject::tr("Save")},
                                     {QMessageBox::Discard, QObject::tr("Discard"), true},
                                     {QMessageBox::Yes, QObject::tr("Yes")},
                                     {QMessageBox::No, QObject::tr("No")},
                                     {QMessageBox::Ok, QObject::tr("OK")},
                                     {QMessageBox::Cancel, QObject::tr("Cancel")}};
    QList<Choice> choices;
    for (const auto& choice : available)
        if (buttons.testFlag(choice.value))
            choices.append(choice);
    if (defaultButton == QMessageBox::NoButton && choices.size() == 1)
        defaultButton = choices.first().value;
    return choose(parent, title, text, choices, defaultButton);
}
std::optional<int> number(QWidget* parent, const QString& title, const QString& label,
                          const int value, const int minimum, const int maximum,
                          const QString& acceptText) {
    Q_ASSERT(parent);
    if (!parent)
        return std::nullopt;
    QPointer<ket::Dialog> dialog = new ket::Dialog(title, parent);
    dialog->setObjectName(QStringLiteral("numberDialog"));
    auto* body = new QWidget(dialog);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* caption = new QLabel(label, body);
    caption->setTextFormat(Qt::PlainText);
    auto* field = new ket::TextField(body);
    field->setObjectName(QStringLiteral("numberField"));
    field->setAccessibleName(label);
    field->setValidator(new QIntValidator(minimum, maximum, field));
    field->setText(QString::number(value));
    field->selectAll();
    layout->addWidget(caption);
    layout->addWidget(field);
    dialog->setBody(body);
    QEventLoop loop;
    std::optional<int> result;
    auto* cancel = dialog->addButton(QObject::tr("Cancel"));
    auto* accept = dialog->addButton(acceptText, ket::ButtonVariant::Primary);
    const auto submit = [&] {
        bool ok = false;
        const int chosen = field->text().toInt(&ok);
        if (!ok || chosen < minimum || chosen > maximum)
            return;
        result = chosen;
        if (dialog)
            dialog->accept();
    };
    QObject::connect(accept, &QAbstractButton::clicked, &loop, submit);
    // Return presses the focused button, as in a QDialog, and submits from the field.
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* enter = new QShortcut(QKeySequence(key), dialog);
        enter->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(enter, &QShortcut::activated, &loop,
                         [cancel, accept] { (cancel->hasFocus() ? cancel : accept)->click(); });
    }
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    QObject::connect(dialog, &ket::Dialog::finished, &loop, &QEventLoop::quit);
    QObject::connect(dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
    dialog->open();
    field->setFocus();
    loop.exec();
    delete dialog.data();
    return result;
}

#define KET_MESSAGE(name)                                                                          \
    QMessageBox::StandardButton name(QWidget* p, const QString& title, const QString& text,        \
                                     QMessageBox::StandardButtons b,                               \
                                     QMessageBox::StandardButton d) {                              \
        return message(p, title, text, b, d);                                                      \
    }
KET_MESSAGE(warning)
KET_MESSAGE(critical)
KET_MESSAGE(information)
KET_MESSAGE(question)
#undef KET_MESSAGE
} // namespace ketplus::message_dialog
