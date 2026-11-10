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
// rename operation instead of creating a widget for every file. Laid out as KetFileRow: the
// folder row above the tree is depth 0, its entries start at depth 1, and folders lead with
// a chevron and read as open while expanded.
class ExplorerRowDelegate final : public QStyledItemDelegate {
  public:
    ExplorerRowDelegate(QFileSystemModel* model, QTreeView* tree,
                        const QHash<QString, QString>& statuses, QObject* parent)
        : QStyledItemDelegate(parent), model_(model), tree_(tree), statuses_(statuses) {}
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
        const bool open = folder && tree_->isExpanded(index);
        const qreal ratio = painter->device()->devicePixelRatioF();
        const int centre = option.rect.center().y();
        int x = option.rect.left() + px(r.indentStart + depth(index) * r.indentStep);
        const int gap = px(folder ? r.folderSpacing : r.spacing);
        if (folder) {
            const int chevron = px(r.chevronSize);
            const QString code =
                ket::Icons::codeForLucide(open ? u"chevron-down" : u"chevron-right");
            painter->drawPixmap(x, centre - chevron / 2,
                                ket::Icons::pixmap(code, chevron, ratio, r.text));
            x += chevron + gap;
        }
        const int icon = px(r.iconSize);
        const QString code = ket::Icons::codeForLucide(!folder ? u"file"
                                                       : open  ? u"folder-open"
                                                               : u"folder");
        painter->drawPixmap(x, centre - icon / 2, ket::Icons::pixmap(code, icon, ratio, r.text));
        x += icon + gap;
        const QString status = statuses_.value(model_->filePath(index));
        QFont statusFont = option.font;
        statusFont.setPixelSize(px(r.statusPixels));
        const int statusWidth =
            status.isEmpty() ? 0 : QFontMetrics(statusFont).horizontalAdvance(status) + gap;
        const int right = option.rect.right() - px(r.paddingInline);
        QFont font = option.font;
        font.setPixelSize(px(r.fontPixels));
        painter->setFont(font);
        const QRect text(x, option.rect.top(), qMax(0, right - statusWidth - x),
                         option.rect.height());
        painter->setPen(r.text);
        painter->drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideMiddle, text.width()));
        if (!status.isEmpty()) {
            painter->setFont(statusFont);
            painter->setPen(r.status);
            painter->drawText(QRect(x, option.rect.top(), right - x, option.rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, status);
        }
        if (r.focusRing.alpha()) {
            painter->setPen(r.focusRing);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
        }
        painter->restore();
    }

  private:
    // Entries of the open folder are depth 1; the folder itself is the row above the tree.
    int depth(QModelIndex index) const {
        int value = 0;
        for (const auto root = tree_->rootIndex(); index.isValid() && index != root;
             index = index.parent())
            ++value;
        return value;
    }

    QFileSystemModel* model_;
    QTreeView* tree_;
    const QHash<QString, QString>& statuses_;
};
} // namespace
void ExplorerPanel::installRowDelegate() {
    tree_->setItemDelegate(new ExplorerRowDelegate(model_, tree_, fileStatuses_, tree_));
    tree_->setMouseTracking(true);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, tree_,
            [this] { tree_->doItemsLayout(); });
    // The chevron and folder icon follow the expanded state.
    connect(tree_, &QTreeView::expanded, tree_->viewport(), qOverload<>(&QWidget::update));
    connect(tree_, &QTreeView::collapsed, tree_->viewport(), qOverload<>(&QWidget::update));
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, tree_->viewport(),
            qOverload<>(&QWidget::update));
}
void ExplorerPanel::setFileStatuses(const QHash<QString, QString>& statuses) {
    fileStatuses_ = statuses;
    tree_->viewport()->update();
}
} // namespace ketplus
