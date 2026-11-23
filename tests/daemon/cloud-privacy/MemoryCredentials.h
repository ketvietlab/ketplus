#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>
namespace r03 {
extern QHash<QString, QByteArray> credentials;
extern bool failWrites;
extern bool failRemovals;
// Fails writes to this account only.
extern QString failWritesTo;
// Fail a selected read after earlier session fields have been read successfully.
extern QString failReadsOf;
// Fails removals of this account only.
extern QString failRemovalsOf;
// Counts writes and removals that changed the store, as the file store rewrites its file once
// for each.
extern int rewrites;
// Accounts read outside a batch view, in order.
extern QStringList reads;
void resetCredentials();
} // namespace r03
