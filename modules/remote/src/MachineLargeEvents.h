#pragma once

#include "MachineBackend.h"

#include <QJsonObject>
#include <QPointer>

namespace ketplus::remote {

// ADR 0119: resolves the `large_event` references of one transcript window or page through
// MachineBackend::transcriptLargeEvent(), so a consumer only ever renders whole events. Items
// are read one after another, each as a serial fragment stream (the daemon's bounded reads),
// and each is checked by LargeEventReassembler (offsets, digest, item id). `done` receives the
// page with every reference replaced by its payload, or an error and no page: a page is never
// rendered with an unresolved or partial event. The backend going away ends it with an error.
void resolveLargeEvents(MachineBackend* backend, const QString& sessionId, qint64 generation,
                        const QJsonObject& page, MachineBackend::Callback done);

} // namespace ketplus::remote
