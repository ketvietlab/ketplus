#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>

namespace ketplus {

// Agents name files in inline code (`src/app.ts:42`, `Pages/Home/index.tsx`,
// `.../hooks.ts:50`). This turns such a span into the link the transcript opens: the path
// from the worktree root, an absolute or home path, or else the one tracked file it ends.
class TranscriptFileLinks final {
  public:
    void setRoot(const QString& root);
    // The link for a code span, "path" or "path:line"; empty when it names no file.
    [[nodiscard]] QString href(const QString& text);

  private:
    [[nodiscard]] QString resolve(const QString& path);
    [[nodiscard]] const QStringList& trackedFiles();

    QString root_;
    QHash<QString, QString> cache_;
    QStringList files_;
    bool filesListed_{false};
    // Files come and go during a turn; answers are re-read after this long.
    QElapsedTimer age_;
};

} // namespace ketplus
