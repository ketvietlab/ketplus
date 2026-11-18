#include "workspace/SidebarControls.h"
#include "KetAppearance.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "workspace/SidebarFilterText.h"
#include <QKeyEvent>
#include <QLabel>
namespace ketplus {
QKeySequence SidebarFilterField::shortcut() { return QKeySequence(QStringLiteral("Ctrl+Alt+K")); }
SidebarFilterField::SidebarFilterField(QWidget* parent)
    : QWidget(parent), field_(new ket::TextField(this)), hint_(new QLabel(field_)) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusProxy(field_);
    field_->setObjectName(QStringLiteral("sidebarRepositoryFilter"));
    field_->setLeadingIcon(ket::Icons::codeForLucide(u"search"));
    field_->setClearButtonEnabled(true);
    field_->setAccessibleName(sidebar_filter::text(sidebar_filter::name));
    field_->setPlaceholderText(sidebar_filter::text(sidebar_filter::placeholder));
    field_->setToolTip(sidebar_filter::text(sidebar_filter::help)
                           .arg(shortcut().toString(QKeySequence::NativeText)));
    field_->setAccessibleDescription(field_->toolTip());
    field_->installEventFilter(this);
    hint_->setText(shortcut().toString(QKeySequence::NativeText));
    hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hint_->setStyleSheet(QStringLiteral("background: transparent;"));
    connect(field_, &QLineEdit::textChanged, this, [this](const QString& text) {
        refresh();
        emit filterTextChanged(text);
    });
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, [this] {
        updateGeometry();
        refresh();
    });
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            [this] { refresh(); });
    refresh();
}
QSize SidebarFilterField::sizeHint() const { return {120, field_->sizeHint().height()}; }
QSize SidebarFilterField::minimumSizeHint() const { return {0, sizeHint().height()}; }
void SidebarFilterField::focusFilter() {
    field_->setFocus(Qt::ShortcutFocusReason);
    field_->selectAll();
}
void SidebarFilterField::refresh() {
    field_->setGeometry(rect());
    const auto& appearance = ket::Appearance::instance();
    const auto recipe = field_->recipe();
    auto font = field_->font();
    font.setPixelSize(qRound(appearance.scaled(11)));
    hint_->setFont(font);
    auto palette = hint_->palette();
    palette.setColor(QPalette::WindowText, recipe.placeholder);
    hint_->setPalette(palette);
    const int padding = qRound(appearance.scaled(recipe.paddingInline));
    const int icon = qRound(appearance.scaled(recipe.iconSize + recipe.spacing));
    hint_->setVisible(field_->text().isEmpty() && !field_->hasFocus());
    hint_->adjustSize();
    hint_->move(width() - padding - hint_->width(), (height() - hint_->height()) / 2);
    field_->setTextMargins(padding - 2 + icon, 0,
                           padding - 2 + (hint_->isHidden() ? 0 : hint_->width() + padding), 0);
}
bool SidebarFilterField::eventFilter(QObject* watched, QEvent* event) {
    if (watched == field_) {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return ||
                key->key() == Qt::Key_Enter) {
                if (event->type() == QEvent::KeyPress && key->key() == Qt::Key_Escape)
                    field_->clear();
                key->accept();
                return true;
            }
        }
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            refresh();
    }
    return QWidget::eventFilter(watched, event);
}
void SidebarFilterField::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh();
}
} // namespace ketplus
