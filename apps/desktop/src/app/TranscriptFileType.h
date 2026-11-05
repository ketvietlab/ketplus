#pragma once

#include <QString>

namespace ketplus {

// Only transcript activation uses this policy; explicit editor/diff actions are unchanged.
bool isTranscriptTextFile(const QString& path);

} // namespace ketplus
