#pragma once

#include <QString>

namespace ketplus::chat_markdown {

// Assistant Markdown as the transcript renders it: every '<' that could open raw HTML outside
// code is backslash-escaped. Qt's Markdown importer hands raw HTML to its HTML parser, and a tag
// that is never closed (a "<title>" placeholder in prose) hides all following text except code
// spans. Code spans, fenced and indented code blocks, autolinks and already escaped characters
// are left as they are, so the escaping never shows. Safe to call from worker threads.
QString escapeRawHtml(const QString& markdown);

// Escape raw HTML and make bare HTTP(S) URLs in prose explicit Markdown autolinks. Preserve
// code and existing links; the stored/copied message remains unchanged. Safe in worker threads.
QString prepareForDisplay(const QString& markdown);

} // namespace ketplus::chat_markdown
