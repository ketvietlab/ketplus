#pragma once

#include <QSet>
#include <QString>

namespace ketplus::daemon {

class CredentialStore;

// What the credential file keeps for each conversation: its content keys
// (`session-key-<sha256(id)>-<generation>-<epoch>`), their wraps for each device
// (`wrapped-<id>-<generation>-<device>-<version>`) and its upload cursor
// (`uploaded-cursor-<id>-<generation>`). Nothing removed them, so the file grew with every
// conversation and a deleted conversation's keys stayed on disk (#84). Keys and wraps of a
// workspace this Mac left are kept as `workspace-keys-<sha256(workspace)>-<account>`.
class SessionCredentials final {
  public:
    explicit SessionCredentials(CredentialStore* store);

    [[nodiscard]] static bool isUploadCursor(const QString& account);
    // Removes every upload cursor, with one rewrite of the file.
    bool clearUploadCursors(QString* error = nullptr) const;
    // Removes the keys, wraps and cursors of conversations that are not in liveSessionIds.
    bool pruneDeleted(const QSet<QString>& liveSessionIds, QString* error = nullptr) const;
    // Makes the content keys in use those of workspaceId, the workspace this Mac is enrolled in.
    // Keys are wrapped for the devices of the workspace they were made in, so another workspace
    // gets its own: the devices of an account this Mac signed out of cannot read what it uploads
    // next (#84). The keys and wraps of the workspace left are set aside under its name, not
    // discarded, and are taken back on returning to it: its relay holds wraps of them and
    // refuses a different wrap under the same id, and its history stays encrypted with them.
    // Upload cursors are not kept. Everything changes in one rewrite. The workspace is recorded
    // beside the enrollment because sign-out removes the enrollment's; keys from before it was
    // recorded are kept in use.
    bool adoptWorkspace(const QString& workspaceId, QString* error = nullptr) const;

  private:
    CredentialStore* store_;
};

} // namespace ketplus::daemon
