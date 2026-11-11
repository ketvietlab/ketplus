#pragma once

#include <QStringList>

namespace ketplus {
// Explicit assistant Markdown links only; no filesystem discovery or execution.
QStringList transcriptAtlasLinks(const QString& markdown);
} // namespace ketplus
