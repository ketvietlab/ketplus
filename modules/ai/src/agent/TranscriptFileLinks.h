#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

namespace ketplus {

// Agents name files in inline code (`src/app.ts:42`, `Pages/Home/index.tsx`,
// `.../hooks.ts:50`). This turns such a span into the link the transcript opens: the path
// from the worktree root, an absolute or home path, or else the one tracked file it ends.
// Formatting runs on the GUI thread, so the worktree's file list is never read here: the owner
// lists it in the background (`requester`) and hands it over with setFiles.
class TranscriptFileLinks final {
  public:
    void setRoot(const QString& root);
    // Asked for a new listing of the current root when one is missing or old.
    void setFileRequester(std::function<void()> requester);
    // Entries of `root` from the background listing; folders end with '/'.
    void setFiles(const QString& root, const QStringList& entries);
    // The link for a code span, "path" or "path:line"; empty when it names no file.
    [[nodiscard]] QString href(const QString& text);

  private:
    // `final` is false when the answer may change once the file list arrives.
    [[nodiscard]] QString resolve(const QString& path, bool* final);
    void requestFiles();

    QString root_;
    QHash<QString, QString> cache_;
    QStringList files_;
    bool filesListed_{false};
    bool filesRequested_{false};
    std::function<void()> requester_;
    // Files come and go during a turn; answers are re-read after this long.
    QElapsedTimer age_;
};

} // namespace ketplus
