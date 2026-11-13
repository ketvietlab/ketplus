#pragma once
#include "providers/acp/AcpToolItem.h"
namespace ketplus::daemon {
inline QJsonObject cursorToolUpdate(QJsonObject previous, const QJsonObject& update) {
    return acpToolUpdate(previous, update);
}
inline QJsonObject cursorToolItem(const QJsonObject& raw) { return acpToolItem(raw); }
} // namespace ketplus::daemon
