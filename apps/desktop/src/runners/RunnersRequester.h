#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace ketplus::runners {

// `error` is empty when the daemon answered; a failed answer carries `error_code` and
// `error_message` in `result`.
using Reply = std::function<void(const QJsonObject& result, const QString& error)>;
// Sends one runner.* request to the daemon.
using Requester = std::function<void(const QString& type, const QJsonObject& payload, Reply)>;

} // namespace ketplus::runners
