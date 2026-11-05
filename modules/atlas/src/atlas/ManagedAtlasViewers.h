#pragma once

#include "workspace/AtlasIndex.h"

#include <QHash>
#include <QObject>
#include <QUrl>

class QProcess;
class QTimer;

namespace ketplus {

// Owns browser viewers independently of the active conversation or worktree.
// KetAtlas --managed closes itself when the last viewer tab goes away.
class ManagedAtlasViewers final : public QObject {
    Q_OBJECT

  public:
    enum class State { Starting, Running, Stopping };
    struct Viewer {
        AtlasDescriptor atlas;
        State state{State::Starting};
        QUrl url;
    };

    static ManagedAtlasViewers* instance();
    [[nodiscard]] QList<Viewer> viewers() const;
    [[nodiscard]] int runningCount() const;
    [[nodiscard]] QString errorFor(const QString& path) const;
    void open(const AtlasDescriptor& atlas);
    void stop(const QString& path);

  signals:
    void changed();

  private:
    explicit ManagedAtlasViewers(QObject* parent);
    void consumeOutput(const QString& path);
    void finish(const QString& path, QProcess* process, const QString& error = {});

    struct ProcessEntry {
        Viewer viewer;
        QProcess* process{nullptr};
        QTimer* timeout{nullptr};
        QByteArray output;
        QByteArray diagnosticTail;
    };
    QHash<QString, ProcessEntry> processes_;
    QHash<QString, QString> errors_;
};

} // namespace ketplus
