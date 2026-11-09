#pragma once
#include <QList>
#include <QVariantList>
#include <QVariantMap>

namespace ketplus::agent_message {

// The files a turn changed, for the changed-files card above its end (mockups/qt
// KetChangedFiles): its file-change rows and the `groupFiles` of its daemon activity groups,
// read back from `end` while rows belong to the same turn. Each file is merged by path in the
// order first changed: { path, destination, operation, added, removed, known }. A failed
// change is left out; counts are known only when every change to the file was counted. A
// steer belongs to the turn it steers; only a prompt starts another turn. `complete` turns
// false when a daemon group listed only some of its files (ADR 0022 `files_total`).
[[nodiscard]] QVariantList turnChangedFiles(const QList<QVariantMap>& rows, qsizetype end,
                                            bool* complete = nullptr);

} // namespace ketplus::agent_message
