#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <functional>
namespace r03 {
extern QHash<QString, QByteArray> credentials;
extern bool failWrites;
extern bool failRemovals;
// Fails writes to this account only.
extern QString failWritesTo;
// Fails removals of this account only.
extern QString failRemovalsOf;
// Counts writes and removals that changed the store, as the file store rewrites its file once
// for each.
extern int rewrites;
void resetCredentials();
} // namespace r03
