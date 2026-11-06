#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
namespace r03 {
extern QHash<QString, QByteArray> credentials;
extern bool failWrites;
extern bool failRemovals;
// Fails writes to this account only.
extern QString failWritesTo;
void resetCredentials();
} // namespace r03
