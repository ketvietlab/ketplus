#pragma once

#include <QString>

namespace ketplus::daemon {

// #125: the fallback when an agent did not call request_user_decision. True when the answer's
// last paragraph, outside code, is a direct question to the user.
[[nodiscard]] bool answerAsksTheUser(const QString& answer);

} // namespace ketplus::daemon
