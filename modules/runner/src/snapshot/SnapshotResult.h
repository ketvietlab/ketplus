#pragma once

#include <QString>

#include <optional>
#include <utility>

// Errors and the result type every snapshot call returns.
namespace ketplus::runner::snapshot {

enum class ErrorCode {
    GitUnavailable, // git could not be started.
    GitTooOld,      // older than minimumGitVersion().
    GitFailed,      // a git command failed; message carries its stderr.
    NotAWorktree,   // the path has no readable .git directory or gitdir file.
    UnsupportedRepository,
    StoreMismatch,      // the snapshot store already belongs to another repository.
    InvalidArgument,    // a malformed object id, relative path or similar.
    BaseMissing,        // the base snapshot of a pack is not in the store.
    PackTooLarge,       // the pack exceeds ApplyLimits::maxPackBytes.
    PackDigestMismatch, // the pack's SHA-256 differs from the expected one.
    PackInvalid,        // git rejected the pack (corrupt or failing fsck checks).
    SnapshotIncomplete, // the commit or an object it reaches is missing after indexing.
    UnsafePath,         // the snapshot tree has a path that could leave the workspace.
    CheckoutTooLarge,   // the snapshot tree exceeds the checkout limits.
    Io,                 // a local file operation failed.
};

struct Error {
    ErrorCode code = ErrorCode::GitFailed;
    QString message;
};

template <typename T> class Result {
  public:
    Result(T value) : m_value(std::move(value)) {}     // NOLINT(google-explicit-constructor)
    Result(Error error) : m_error(std::move(error)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] const T& value() const { return *m_value; }
    [[nodiscard]] T& value() { return *m_value; }
    [[nodiscard]] const Error& error() const { return m_error; }

  private:
    std::optional<T> m_value;
    Error m_error;
};

} // namespace ketplus::runner::snapshot
