#include "cloud/CloudControlSupport.h"

#include "cloud/WorktreeRepositories.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>

namespace ketplus::daemon::cloud_control {

QString opaquePathId(const QByteArray& kind, const QString& path, const QByteArray& salt) {
    if (salt.isEmpty())
        return {};
    return QString::fromLatin1(QMessageAuthenticationCode::hash(kind + '\n' + path.toUtf8(), salt,
                                                                QCryptographicHash::Sha256)
                                   .toHex());
}

QJsonObject sourceContext(const QString& worktreePath, const QByteArray& salt,
                          const QString& recordedRepository) {
    const QString canonical = QFileInfo(worktreePath).canonicalFilePath();
    const QString worktree =
        canonical.isEmpty() ? QFileInfo(worktreePath).absoluteFilePath() : canonical;
    const QString gitDirectory = worktreeGitDirectory(worktree);
    QString repository = worktree;
    if (const qsizetype linked = gitDirectory.indexOf(QStringLiteral("/.git/worktrees/"));
        linked > 0) {
        repository = gitDirectory.left(linked);
    } else if (gitDirectory.isEmpty() && !recordedRepository.isEmpty()) {
        repository = recordedRepository;
    }
    QString branch;
    QFile head(QDir(gitDirectory).filePath(QStringLiteral("HEAD")));
    if (!gitDirectory.isEmpty() && head.open(QIODevice::ReadOnly)) {
        branch = QString::fromUtf8(head.readLine()).trimmed();
        const QString referencePrefix = QStringLiteral("ref: refs/heads/");
        if (branch.startsWith(referencePrefix)) {
            branch = branch.mid(referencePrefix.size());
        } else if (branch.size() > 12) {
            branch = branch.left(12);
        }
    }
    const QString repositoryLabel = QFileInfo(repository).fileName();
    const QString worktreeLabel = QFileInfo(worktree).fileName();
    return {{QStringLiteral("repositoryId"), opaquePathId("repository", repository, salt)},
            {QStringLiteral("repositoryLabel"),
             repositoryLabel.isEmpty() ? worktreeLabel : repositoryLabel},
            {QStringLiteral("worktreeId"), opaquePathId("worktree", worktree, salt)},
            {QStringLiteral("worktreeLabel"), worktreeLabel},
            {QStringLiteral("branch"), branch}};
}

} // namespace ketplus::daemon::cloud_control
