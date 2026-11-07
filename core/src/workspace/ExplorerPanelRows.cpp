#include "KetAppearance.h"
#include "KetIcons.h"
#include "KetSurfaceRecipes.h"
#include "KetTextField.h"
#include "workspace/ExplorerPanel.h"
#include <QFileSystemModel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeView>
namespace ketplus {
namespace {
// FileRow recipes on a virtualized filesystem model; inline edits keep QFileSystemModel's
// rename operation instead of creating a widget for every file.
class ExplorerRowDelegate final : public QStyledItemDelegate {
  public:
    ExplorerRowDelegate(QFileSystemModel* model, const QHash<QString, QString>& statuses,
                        QObject* parent)
        : QStyledItemDelegate(parent), model_(model), statuses_(statuses) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        auto& a = ket::Appearance::instance();
        const auto r = ket::fileRowRecipe(false, {}, a.theme());
        return {0, qRound(a.scaled(model_->isDir(index) ? r.folderHeight : r.height))};
    }
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        return new ket::TextField(parent);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto& a = ket::Appearance::instance();
        const auto px = [&a](qreal v) { return qRound(a.scaled(v)); };
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const auto r = ket::fileRowRecipe(selected,
                                          {option.state.testFlag(QStyle::State_MouseOver), false,
                                           true, option.state.testFlag(QStyle::State_HasFocus)},
                                          a.theme());
        painter->save();
        painter->setClipRect(option.rect);
        painter->fillRect(option.rect, r.background);
        const bool folder = model_->isDir(index);
        const QString code = ket::Icons::codeForLucide(folder ? u"folder" : u"file");
        const int icon = px(r.iconSize), left = option.rect.left() + px(r.spacing);
        painter->drawPixmap(
            left, option.rect.center().y() - icon / 2,
            ket::Icons::pixmap(code, icon, painter->device()->devicePixelRatioF(), r.text));
        QFont font = option.font;
        font.setPixelSize(px(r.fontPixels));
        painter->setFont(font);
        const QString status = statuses_.value(model_->filePath(index));
        const int statusWidth = status.isEmpty() ? 0 : px(24);
        const QRect text = option.rect.adjusted(px(r.spacing) + icon + px(r.spacing), 0,
                                                -px(r.spacing) - statusWidth, 0);
        painter->setPen(r.text);
        painter->drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        if (!status.isEmpty()) {
            painter->setPen(r.status);
            painter->drawText(QRect(option.rect.right() - statusWidth, option.rect.top(),
                                    statusWidth, option.rect.height()),
                              Qt::AlignCenter, status);
        }
        if (r.focusRing.alpha()) {
            painter->setPen(r.focusRing);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
        }
        painter->restore();
    }

  private:
    QFileSystemModel* model_;
    const QHash<QString, QString>& statuses_;
};
} // namespace
void ExplorerPanel::installRowDelegate() {
    tree_->setItemDelegate(new ExplorerRowDelegate(model_, fileStatuses_, tree_));
    tree_->setMouseTracking(true);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, tree_, [this] {
        tree_->setIndentation(qRound(ket::Appearance::instance().scaled(14)));
        tree_->doItemsLayout();
    });
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, tree_->viewport(),
            qOverload<>(&QWidget::update));
}
void ExplorerPanel::setFileStatuses(const QHash<QString, QString>& statuses) {
    fileStatuses_ = statuses;
    tree_->viewport()->update();
}
} // namespace ketplus
