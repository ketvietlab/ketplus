#include "git/GitService.h"
#include <QDir>
#include <algorithm>
namespace ketplus {
void GitService::stageFile(const QString& path) { mutateFile(path, true); }
void GitService::unstageFile(const QString& path) { mutateFile(path, false); }
void GitService::mutateFile(const QString& path, bool stage) {
    const QString relative = QDir(repositoryRoot_).relativeFilePath(canonicalEntryPath(path));
    const auto found =
        std::find_if(snapshot_.files.cbegin(), snapshot_.files.cend(),
                     [&](const GitFileStatus& file) { return file.path == relative; });
    const QString operation = stage ? QStringLiteral("stage") : QStringLiteral("unstage");
    if (repositoryRoot_.isEmpty() || QDir::isAbsolutePath(relative) || relative == ".." ||
        relative.startsWith("../") || found == snapshot_.files.cend()) {
        emit mutationFinished(operation, false,
                              tr("Refresh Source Control before changing this file."));
        return;
    }
    const bool unborn = snapshot_.head.isEmpty() || snapshot_.head == QStringLiteral("(initial)");
    QStringList args = stage    ? QStringList{"add"}
                       : unborn ? QStringList{"rm", "--cached", "--ignore-unmatch"}
                                : QStringList{"restore", "--staged"};
    args << "--" << QStringLiteral(":(literal)%1").arg(relative);
    if (!found->originalPath.isEmpty())
        args << QStringLiteral(":(literal)%1").arg(found->originalPath);
    runMutation(operation, args);
}
void GitService::commitStaged(const QString& message) {
    const bool staged = std::any_of(snapshot_.files.cbegin(), snapshot_.files.cend(),
                                    [](const auto& file) { return file.hasStagedChange(); });
    if (message.trimmed().isEmpty() || !staged) {
        emit mutationFinished(QStringLiteral("commit"), false,
                              tr("Enter a commit message and stage at least one change."));
        return;
    }
    runMutation(QStringLiteral("commit"), {"commit", "-m", message.trimmed()});
}
void GitService::runMutation(const QString& operation, const QStringList& arguments) {
    if (mutationRunning_ || repositoryRoot_.isEmpty() || !isAvailable()) {
        emit mutationFinished(operation, false,
                              tr("Git is unavailable or another Git action is running."));
        return;
    }
    mutationRunning_ = true;
    emit mutationRunningChanged(true);
    const quint64 generation = workspaceGeneration_;
    const QString root = repositoryRoot_;
    QStringList args{"-C", root};
    args.append(arguments);
    runGit(
        args, root,
        [this, generation, operation](int exitCode, const QByteArray&, const QByteArray& error,
                                      bool completed) {
            mutationRunning_ = false;
            emit mutationRunningChanged(false);
            if (generation != workspaceGeneration_)
                return;
            const bool ok = completed && exitCode == 0;
            emit mutationFinished(
                operation, ok,
                ok                ? QString{}
                : error.isEmpty() ? tr("Git did not complete the action. Refresh and try again.")
                                  : QString::fromUtf8(error).trimmed());
            refresh();
        },
        120000);
}
} // namespace ketplus
