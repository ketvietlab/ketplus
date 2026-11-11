#pragma once

#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWidget>

namespace ket {
class ConversationRow;
}
class QVBoxLayout;

namespace ketplus {

// One repository in the product sidebar. Conversations are the navigation unit;
// each conversation map carries the worktree it runs in as "path" and whether it
// is the window's current conversation as "selected".
struct SidebarRepository final {
    QString path;
    QString name;
    bool active{false};
    QVariantList conversations;
    bool promoted{false};

    friend bool operator==(const SidebarRepository& left, const SidebarRepository& right) {
        return left.path == right.path && left.name == right.name && left.active == right.active &&
               left.conversations == right.conversations && left.promoted == right.promoted;
    }
};

// The order the phones use too (ketplus-app MobileWorkspaceData): conversations by
// "lastTurnStartedAt", newest first, then title and id; repositories by their newest
// conversation, then name and path. An explicit conversation repository switch promotes its target.
void sortSidebarRepositories(QVector<SidebarRepository>& repositories);

class SidebarRepositoryList final : public QWidget {
    Q_OBJECT

  public:
    explicit SidebarRepositoryList(QWidget* parent = nullptr);

    void applyZoom(int zoomPercent);
    void setTagFilter(const QString& tag);
    void setRepositories(const QVector<SidebarRepository>& repositories);

  signals:
    // A conversation changed visual status without rebuilding its row; the owner redraws its
    // Két Symbol pixmap using the current palette.
    void iconPresentationChanged();
    void repositoryRequested(const QString& path);
    void newConversationRequested(const QString& repositoryPath);
    void conversationRenameRequested(const QString& worktreePath, const QString& conversationId);
    void conversationRequested(const QString& worktreePath, const QString& conversationId);
    void conversationContextMenuRequested(const QString& worktreePath,
                                          const QString& conversationId);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void markConversationSeen(const QString& conversationId);
    void updateRows(const QVector<SidebarRepository>& repositories);
    void applyPresentation();
    void onRepositoryHeaderClicked(const QString& path);
    void persistCollapsedRepositories() const;

    QVBoxLayout* layout_{nullptr};
    QVector<SidebarRepository> repositories_;
    QStringList structure_;
    QVector<ket::ConversationRow*> rows_;
    QVector<QWidget*> headers_;
    QVector<QWidget*> groups_;
    QSet<QString> collapsedPaths_;
    QSet<QString> showAllPaths_;
    // Completion is new until the user opens that conversation during this application session.
    QSet<QString> seenConversationIds_;
    QString tagFilter_;
    bool built_{false};
};

} // namespace ketplus
