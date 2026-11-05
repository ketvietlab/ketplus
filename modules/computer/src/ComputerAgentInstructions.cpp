#include "ComputerAgentInstructions.h"

namespace ketplus::computer_tools {

QString agentInstructions(const bool computerAvailable) {
    const QString status =
        computerAvailable
            ? QStringLiteral(
                  "KetPlus supplied the ketplus_computer MCP server for this conversation. When "
                  "the KetPlus browser policy allows computer use (Safari, or native desktop apps "
                  "that are not web browsers), use only its tools. Tool names may have the "
                  "mcp__ketplus_computer__ prefix. Prefer an MCP server, a shell command or "
                  "ketplus_browser when one can do the task. Call list_apps, then request_access "
                  "for the apps you need with a one-sentence reason; the user approves them. "
                  "The tools work in the background: the user keeps their pointer, keyboard and "
                  "front app, and granted apps work even behind other windows. Granted apps "
                  "move to the work display, a secondary display when one is connected, and "
                  "screenshots show it by default. When a screenshot says an app is covered, "
                  "its pixels may be old: trust observe for its state. Prefer observe "
                  "and act on element refs; left_click, right_click, scroll, type and key at "
                  "screenshot coordinates also run in the background. Take a screenshot or "
                  "observe again to verify each important step; any input makes refs stale. "
                  "Use \"foreground\": true only after NEEDS_FOREGROUND, because it moves the "
                  "user's real pointer. Terminals and IDEs accept clicks only, and browsers "
                  "other than Safari are view-only. On CANCELLED, REMOTE_TURN, SECURE_INPUT or "
                  "CONTROL_LOCKED, stop using the computer and tell the user. Never type "
                  "passwords or other secrets; ask the user to enter them.")
            : QStringLiteral(
                  "KetPlus computer use is off or unavailable for this conversation. If a task "
                  "needs control of a native app, report that limitation and the need to enable "
                  "Settings > Computer use, and continue work that does not need it.");
    return QStringLiteral("[KetPlus computer policy]\n") + status +
           QStringLiteral(
               "\nDo not drive the desktop by any other means: no provider-native computer use, "
               "AppleScript or osascript UI scripting, cliclick or other input automation."
               "\n[/KetPlus computer policy]");
}

} // namespace ketplus::computer_tools
