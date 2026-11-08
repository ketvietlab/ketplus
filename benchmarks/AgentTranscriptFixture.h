#pragma once
#include <QString>
#include <QVariantList>

namespace ketplus::benchmark {

// The message rows of the mock's workspace/<provider> transcript (mockups/qt
// TranscriptFixtures.qml), projected from synthetic journal events the way the daemon's
// rows are, for native app/mock captures. `assets` holds the mock's screenshots.
QVariantList transcriptFixture(const QString& provider, const QString& assets);

} // namespace ketplus::benchmark
