#include "runners/RunnerDialogText.h"

#include "KetAppearance.h"
#include "KetRecipes.h"

#include <QLabel>
#include <QWidget>

namespace ketplus::runners::dialog_text {
namespace {

const char* const roleProperty = "runnerTextRole";

void paint(QWidget* root) {
    const ket::Appearance& appearance = ket::Appearance::instance();
    const ket::FieldRowRecipe recipe = ket::fieldRowRecipe(appearance.theme());
    for (QLabel* label : root->findChildren<QLabel*>()) {
        const QVariant value = label->property(roleProperty);
        if (!value.isValid())
            continue;
        const auto role = Role(value.toInt());
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, role == Role::Error  ? recipe.error
                                               : role == Role::Help ? recipe.help
                                                                    : recipe.label);
        label->setPalette(palette);
        QFont font = label->font();
        const qreal pixels = role == Role::Code   ? recipe.labelPixels * 1.6
                             : role == Role::Help ? recipe.helpPixels
                                                  : recipe.labelPixels;
        font.setPixelSize(qMax(1, qRound(appearance.scaled(pixels))));
        font.setBold(role == Role::Code);
        if (role == Role::Code)
            font.setLetterSpacing(QFont::AbsoluteSpacing, appearance.scaled(2));
        label->setFont(font);
    }
}

} // namespace

QLabel* make(const QString& text, const Role role, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    label->setWordWrap(role != Role::Code);
    label->setTextInteractionFlags(role == Role::Code ? Qt::TextSelectableByMouse
                                                      : Qt::NoTextInteraction);
    label->setProperty(roleProperty, int(role));
    return label;
}

void follow(QWidget* root) {
    paint(root);
    ket::Appearance& appearance = ket::Appearance::instance();
    QObject::connect(&appearance, &ket::Appearance::themeChanged, root, [root] { paint(root); });
    QObject::connect(&appearance, &ket::Appearance::zoomChanged, root, [root] { paint(root); });
}

} // namespace ketplus::runners::dialog_text
