#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QQuickView;

namespace ketplus {

// The details pane's Changes tab (mock DetailsPane, changes): the uncommitted files of the
// conversation's worktree as the kit changed-files card, each opening its diff, with the way to
// Source Control. The header's Changes opens it.
class ConversationChangesPane final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ files NOTIFY changesChanged)
    Q_PROPERTY(QString note READ note NOTIFY changesChanged)

  public:
    explicit ConversationChangesPane(QWidget* parent = nullptr);
    ~ConversationChangesPane() override;

    [[nodiscard]] QVariantList files() const { return files_; }
    [[nodiscard]] QString note() const { return note_; }
    [[nodiscard]] QString root() const { return root_; }
    [[nodiscard]] QQuickView* quickView() const { return view_; }

    // The worktree's uncommitted paths, relative to `root`, and its branch for the note.
    void setChanges(const QString& root, const QString& branch, const QStringList& paths);

    Q_INVOKABLE void activateFile(int index);
    Q_INVOKABLE void openSourceControl() { emit sourceControlRequested(); }

  signals:
    void changesChanged();
    void fileDiffRequested(const QString& absolutePath);
    void sourceControlRequested();

  private:
    QQuickView* view_{nullptr};
    QString root_;
    QStringList paths_;
    QVariantList files_;
    QString note_;
};

} // namespace ketplus
