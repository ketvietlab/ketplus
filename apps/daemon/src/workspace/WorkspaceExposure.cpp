#include "workspace/WorkspaceExposure.h"

#include <QSet>
#include <QStringList>

namespace ketplus::daemon::workspace_exposure {

bool isSecretPath(const QString& relativePath) {
    static const QSet<QString> names{QStringLiteral(".netrc"),
                                     QStringLiteral(".npmrc"),
                                     QStringLiteral(".pypirc"),
                                     QStringLiteral(".pgpass"),
                                     QStringLiteral(".git-credentials"),
                                     QStringLiteral("id_rsa"),
                                     QStringLiteral("id_dsa"),
                                     QStringLiteral("id_ecdsa"),
                                     QStringLiteral("id_ed25519"),
                                     QStringLiteral("credentials.json"),
                                     QStringLiteral("service-account.json")};
    static const QSet<QString> suffixes{QStringLiteral("pem"),      QStringLiteral("key"),
                                        QStringLiteral("p12"),      QStringLiteral("pfx"),
                                        QStringLiteral("keystore"), QStringLiteral("jks")};
    // Templates are meant to be shared and hold placeholders, not values.
    static const QSet<QString> templates{QStringLiteral(".env.example"),
                                         QStringLiteral(".env.sample"),
                                         QStringLiteral(".env.template")};
    const QStringList segments = relativePath.split(u'/', Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        const QString name = segment.toLower();
        if (templates.contains(name))
            continue;
        if (name == QStringLiteral(".env") || name.startsWith(QStringLiteral(".env.")) ||
            names.contains(name))
            return true;
        const qsizetype dot = name.lastIndexOf(u'.');
        if (dot > 0 && suffixes.contains(name.mid(dot + 1)))
            return true;
    }
    return false;
}

} // namespace ketplus::daemon::workspace_exposure
