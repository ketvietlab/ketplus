#pragma once

#include <QString>

namespace ketplus::agent_message {

// A shell token without its matching outer quotes.
[[nodiscard]] QString unquoteShellToken(QString value);
// A path operand without its quotes, or the close of a `zsh -lc "..."` wrapper.
[[nodiscard]] QString pathOperand(QString value);
// Where a transcript row opens a file: an absolute path, with `:line` when the read starts
// past the top. A relative path resolves from a leading `cd <dir> &&`, also inside a
// `zsh -lc "..."` wrapper, else from `cwd`; empty when neither gives a base.
[[nodiscard]] QString fileLocation(const QString& path, int line, const QString& command,
                                   const QString& cwd);

} // namespace ketplus::agent_message
