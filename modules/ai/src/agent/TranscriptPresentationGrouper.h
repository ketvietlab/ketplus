#pragma once

#include <QVariantList>

namespace ketplus {

// Builds the UI projection from canonical entries without mutating the source list.
class TranscriptPresentationGrouper final {
  public:
    [[nodiscard]] static QVariantList group(const QVariantList& canonicalItems);
};

} // namespace ketplus
