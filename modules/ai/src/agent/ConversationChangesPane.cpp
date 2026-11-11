#include "agent/ConversationChangesPane.h"

#include "KetAppearance.h"
#include "KetTokens.h"
#include "ui/InterfaceFont.h"

#include <QDir>
#include <QQmlContext>
#include <QQuickView>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>

namespace ketplus {

ConversationChangesPane::ConversationChangesPane(QWidget* parent)
    : QWidget(parent), view_(new QQuickView) {
    setObjectName(QStringLiteral("conversationChangesPane"));
    auto* container = QWidget::createWindowContainer(view_, this);
    container->setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(container);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    const auto applyTheme = [this] {
        view_->setColor(ket::Tokens::color(u"page-bg", ket::Appearance::instance().theme()));
    };
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this, applyTheme);
    applyTheme();
    view_->rootContext()->setContextProperty(QStringLiteral("changesHost"), this);
    static_cast<void>(InterfaceUiFont::resolvedFamily());
    view_->setSource(QUrl(QStringLiteral("qrc:/agent/ConversationChangesPane.qml")));
}

// The scene goes before this host, so no binding reads a destroyed changesHost.
ConversationChangesPane::~ConversationChangesPane() { view_->setSource(QUrl()); }

void ConversationChangesPane::setChanges(const QString& root, const QString& branch,
                                         const QStringList& paths) {
    QVariantList files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        const int split = path.lastIndexOf(QLatin1Char('/'));
        // Git status gives no line counts here, so the card shows none.
        files.append(QVariantMap{{QStringLiteral("name"), path.mid(split + 1)},
                                 {QStringLiteral("path"), split > 0 ? path.left(split) : QString()},
                                 {QStringLiteral("known"), false}});
    }
    QString note;
    if (!paths.isEmpty())
        note = branch.isEmpty() ? tr("Not committed") : tr("%1 · not committed").arg(branch);
    if (root == root_ && paths == paths_ && note == note_)
        return;
    root_ = root;
    paths_ = paths;
    files_ = files;
    note_ = note;
    emit changesChanged();
}

void ConversationChangesPane::activateFile(const int index) {
    if (index < 0 || index >= paths_.size() || root_.isEmpty())
        return;
    emit fileDiffRequested(QDir(root_).filePath(paths_.at(index)));
}

} // namespace ketplus
