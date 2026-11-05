#include "BrowserAgentInstructions.h"

#include "BundledAgentSkills.h"

namespace ketplus::browser_tools {

QString browserAgentInstructions(const bool available) {
    const QString status =
        available
            ? QStringLiteral(
                  "KetPlus has supplied the ketplus_browser MCP server for this session. "
                  "For tasks requiring a real browser (opening web pages, interacting with web UI, "
                  "checking rendered layouts, screenshots or visual comparisons), use its "
                  "browser_* "
                  "and visual_compare tools. Tool names may have the mcp__ketplus_browser__ "
                  "prefix. "
                  "Start with browser_capabilities if needed, then browser_create_session, "
                  "browser_open, browser_observe, browser_act, and observe again. Use nodeRef and "
                  "snapshotId from the latest observation; every action invalidates that snapshot. "
                  "Use browser_wait for readiness. "
                  "Debug with browser_logs, browser_network and browser_inspect; they are "
                  "read-only and keep the snapshot. "
                  "Use matching viewport/theme and observations with screenshots for "
                  "visual_compare. "
                  "Screenshots: check state, text and layout with observe bounds and "
                  "browser_inspect first; capture one image per distinct state that needs a "
                  "visual check, never after every action, and about five per turn unless the "
                  "user asks for more. For a state/viewport/theme matrix, verify each cell by "
                  "measurement or visual_compare and capture only failing cells or ones the user "
                  "must see. The user already sees each capture in the transcript. "
                  "Screenshot files: browser_capture returns path; browser_observe with "
                  "includeScreenshot returns capturePath. Use or copy these original PNG files "
                  "as assets or evidence for the authorized task, respecting normal filesystem "
                  "permissions. Copy needed files into the project before closing the session; "
                  "temporary captures are deleted on close. Do not reconstruct image files from "
                  "base64/data URIs in tool output or dump image bytes into text. Record evidence "
                  "as text (URL, viewport, theme, state, captureId, measurements, visual_compare "
                  "findings), and use the returned file path when a screenshot asset is needed. "
                  "If a worker returns no file path, report that its runtime needs updating; "
                  "never invent a path. "
                  "Mandatory cleanup: track every sessionId you create. You MUST call "
                  "browser_close_session for every session you created as soon as browser work "
                  "ends and before your final response, including after errors or blocked or "
                  "abandoned work. Close unused pages promptly; closing a page alone does not "
                  "close its session. Preserve needed evidence before closing: session closure "
                  "deletes captures. Verify each close succeeds; if cleanup fails or tools become "
                  "unavailable, report the unclosed sessionIds instead of claiming cleanup. "
                  "KetPlus closes sessions still open when a turn ends; none carries into the next "
                  "turn. Do not close "
                  "user-owned or other agents' sessions or kill shared browser processes.")
            : QStringLiteral(
                  "KetPlus browser MCP tools are disabled or unavailable for this session. "
                  "If the task needs a real browser, report that limitation and the need to enable "
                  "or repair Settings > Browser, unless it is a Safari task allowed below. "
                  "Continue "
                  "work that does not need a browser. "
                  "Do not claim to have inspected, interacted with or captured a page.");
    return QStringLiteral("[KetPlus browser policy]\n") + status +
           QStringLiteral(
               "\nApart from the Safari case below, do not discover or switch to a "
               "provider-native browser, another browser MCP, raw Playwright/Puppeteer, browser "
               "automation through shell commands, an ad-hoc browser script or computer use. If "
               "KetPlus tools are missing, fail, or cannot support the operation, report the "
               "limitation instead of falling back. Respect the host's allowed origins: on "
               "NAVIGATION_DENIED, REQUEST_DENIED or ORIGIN_NOT_PERMITTED, report the required "
               "origin; do not widen access or bypass the policy through any other browser."
               "\nComputer use (desktop screen and input control) is allowed in two cases. "
               "(1) Safari: ketplus_browser drives Chromium only, so when a task specifically "
               "needs Safari or WebKit (Safari-only behaviour, rendering or bugs, or the user "
               "asks for Safari), drive Safari with computer use and say why. "
               "(2) Native desktop applications that are not web browsers. "
               "Never use computer use to drive Chrome, Edge, Firefox or another browser in place "
               "of ketplus_browser. "
               "This policy supersedes earlier KetPlus browser availability guidance."
               "\n[/KetPlus browser policy]");
}

QString agentReadingInstructions() {
    return QStringLiteral(
        "[KetPlus reading policy]\n"
        "Every byte a tool prints stays in the conversation context, so read narrowly. "
        "Search with rg (file names with rg --files or rg -l, text with rg -n) before opening "
        "files, then read only the line ranges you need (for example sed -n '120,180p' file) "
        "instead of whole files. Do not cat several files at once, dump whole directories or "
        "print large generated, minified or lock files. Do not list or print available tools, "
        "MCP schemas or tool descriptions; call the tool you need. Cap long command output with "
        "head, tail or rg filters. A browser observation is a summary by default; request "
        "detail \"full\" only when the summary lacks what the task needs."
        "\n[/KetPlus reading policy]");
}

QString agentMermaidInstructions() {
    return QStringLiteral(
        "[KetPlus Mermaid guidance]\n"
        "When a Mermaid diagram helps explain the task, make it compact and readable. "
        "Preserve the meaning, arrow direction and important relationships; do not delete "
        "dependencies just to make the layout prettier. Follow the user's requested notation. "
        "For architecture diagrams, prefer a consistent left-to-right flow: clients, services, "
        "then storage/external systems. Use top-to-bottom when it better fits a process. "
        "Group nodes by responsibility in a few shallow subgraphs. Order declarations and "
        "edges to minimize crossings and long return lines. Separate secondary/failover "
        "paths with clearly labelled dashed edges. "
        "Keep node labels short (usually one to three lines), put details in adjacent prose, "
        "and label edges only when their meaning is not obvious. Split an overloaded diagram "
        "into an overview and focused diagrams rather than shrinking text or dropping facts. "
        "Use a small consistent set of shapes and semantic styles; prefer the host theme, "
        "avoid hard-coded backgrounds or low-contrast text, and do not use emoji as node icons. "
        "Use standard Mermaid syntax supported by the renderer; do not assume optional "
        "layout plugins are installed. Quote labels containing punctuation. "
        "Before presenting, check syntax, grouping, crossings, label length and whether the "
        "main flow is easy to follow. If a rendering tool is available, inspect the rendered "
        "diagram and refine it; do not claim visual verification without rendering. "
        "Do not install tools or call another paid model merely to polish a diagram."
        "\n[/KetPlus Mermaid guidance]");
}

QString hostAgentInstructions(const bool browserAvailable) {
    QString text = browserAgentInstructions(browserAvailable) + QStringLiteral("\n\n") +
                   agentReadingInstructions() + QStringLiteral("\n\n") + agentMermaidInstructions();
    if (const QString skills = agent_skills::defaultSkillInstructions(); !skills.isEmpty())
        text += QStringLiteral("\n\n") + skills;
    return text;
}

QJsonObject withCodexBrowserInstructions(const QString& method, QJsonObject params,
                                         const bool available) {
    if (method != QStringLiteral("thread/start") && method != QStringLiteral("thread/resume"))
        return params;
    const QString existing = params.value(QStringLiteral("developerInstructions")).toString();
    params.insert(QStringLiteral("developerInstructions"),
                  existing.isEmpty()
                      ? hostAgentInstructions(available)
                      : existing + QStringLiteral("\n\n") + hostAgentInstructions(available));
    return params;
}

} // namespace ketplus::browser_tools
