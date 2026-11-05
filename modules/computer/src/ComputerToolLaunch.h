#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::computer_tools {

// ADR 0030. The KetPlus Computer agent app enforces access; this module decides whether a
// conversation gets the stdio front at all and mints the token the agent checks.
inline constexpr auto serverName = "ketplus_computer";
inline constexpr auto agentAppName = "KetPlus Computer.app";
inline constexpr auto frontExecutable = "Contents/MacOS/ketplus-computer-mcp";

struct Settings final {
    // Opt-in: desktop control stays off until the user enables it.
    bool enabled{false};
    // Bundle IDs the agent must never capture or control, on top of its built-in tiers.
    QStringList deniedApps;
    // The agent's work display (ADR 0032): "secondary" (a display other than the main one,
    // falling back to the main display) or "main".
    QString display{QStringLiteral("secondary")};
};

struct Launch final {
    QString command;
    QStringList args;
    QHash<QString, QString> env;
};

// <KetPlus data>/computer-tools.json, or KETPLUS_COMPUTER_TOOLS_CONFIG.
[[nodiscard]] QString settingsFilePath();
// <KetPlus data>/computer (0700), or KETPLUS_COMPUTER_DIR. Shared with the agent app.
[[nodiscard]] QString agentDirectory();
// Missing or unreadable files yield the defaults (disabled).
[[nodiscard]] Settings loadSettings(const QString& path = settingsFilePath());
// Keeps unknown keys; writes atomically with owner-only permissions.
bool saveSettings(const Settings& settings, const QString& path = settingsFilePath());

// KETPLUS_COMPUTER_APP (an app bundle) first, then the app nested beside the
// executable: Contents/Helpers for the daemon, ../Helpers for the desktop app.
[[nodiscard]] QString findFrontExecutable(const QString& applicationDir);

// 32 random bytes in <directory>/host.key, created 0600 on first use.
[[nodiscard]] QByteArray ensureHostKey(const QString& directory = agentDirectory());
// hex(HMAC-SHA256(key, "computer.v0:" + conversation)), matching ConversationToken.swift.
[[nodiscard]] QString mintToken(const QByteArray& key, const QString& conversationId);
// First 32 hex digits of SHA-256(conversation), the agent's per-conversation file name.
[[nodiscard]] QString fileStem(const QString& conversationId);

// The front for one conversation, or nullopt when disabled, missing or unkeyed.
[[nodiscard]] std::optional<Launch>
launchForConversation(const Settings& settings, const QString& conversationId,
                      const QString& applicationDir, const QString& directory = agentDirectory());
[[nodiscard]] std::optional<Launch> launchFromUserSettings(const QString& conversationId,
                                                           const QString& applicationDir);

// Merges this server into a Claude SDK mcpServers map / an ACP mcpServers list.
[[nodiscard]] QJsonObject withClaudeMcpServer(QJsonObject servers,
                                              const std::optional<Launch>& launch);
[[nodiscard]] QJsonArray withAcpMcpServer(QJsonArray servers, const std::optional<Launch>& launch);

// While a remote client's turn runs, the agent refuses this conversation (ADR 0030).
bool setRemoteTurn(const QString& conversationId, bool remote,
                   const QString& directory = agentDirectory());

} // namespace ketplus::computer_tools
