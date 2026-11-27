#include "MachineLargeEvents.h"

#include "LargeEventReassembler.h"

#include <QJsonArray>

#include <memory>
#include <optional>
#include <utility>

namespace ketplus::remote {

namespace {

// Bounded fragment reads, as the local Agent Workspace asks for them.
constexpr int maximumFragmentBytes = 128 * 1024;

struct Resolution {
    QPointer<MachineBackend> backend;
    QString sessionId;
    qint64 generation{0};
    QJsonObject page;
    QJsonArray events;
    qsizetype index{0};
    std::optional<LargeEventReassembler> assembler;
    MachineBackend::Callback done;
};

void advance(const std::shared_ptr<Resolution>& state);

void finish(const std::shared_ptr<Resolution>& state, const QJsonObject& page,
            const QString& error) {
    auto done = std::exchange(state->done, nullptr);
    if (done)
        done(page, error);
}

void fetch(const std::shared_ptr<Resolution>& state) {
    if (state->backend == nullptr) {
        finish(state, {}, QStringLiteral("This machine is no longer connected."));
        return;
    }
    state->backend->transcriptLargeEvent(
        state->sessionId, state->generation,
        {{QStringLiteral("transfer_id"), state->assembler->transferId()},
         {QStringLiteral("offset"), state->assembler->nextOffset()},
         {QStringLiteral("maximum_bytes"), maximumFragmentBytes}},
        [state](const QJsonObject& fragment, const QString& fragmentError) {
            if (!fragmentError.isEmpty()) {
                finish(state, {}, fragmentError);
                return;
            }
            QString error;
            if (fragment.value(QLatin1String("session_id")).toString() != state->sessionId ||
                fragment.value(QLatin1String("session_generation")).toInteger() !=
                    state->generation ||
                !state->assembler->accept(fragment, &error)) {
                finish(state, {},
                       error.isEmpty()
                           ? QStringLiteral("Conflicting or incomplete large transcript fragment.")
                           : error);
                return;
            }
            if (!state->assembler->isComplete()) {
                fetch(state);
                return;
            }
            const QJsonObject payload = state->assembler->payload(&error);
            if (payload.isEmpty()) {
                finish(state, {}, error);
                return;
            }
            QJsonObject event = state->events.at(state->index).toObject();
            event.insert(QStringLiteral("payload"), payload);
            state->events.replace(state->index, event);
            state->assembler.reset();
            ++state->index;
            advance(state);
        });
}

void advance(const std::shared_ptr<Resolution>& state) {
    for (; state->index < state->events.size(); ++state->index) {
        const QJsonObject event = state->events.at(state->index).toObject();
        const QJsonObject reference = event.value(QLatin1String("payload"))
                                          .toObject()
                                          .value(QLatin1String("large_event"))
                                          .toObject();
        if (reference.isEmpty())
            continue;
        state->assembler.emplace(reference);
        if (!state->assembler->isValid() ||
            state->assembler->eventType() != event.value(QLatin1String("event_type")).toString()) {
            finish(state, {}, QStringLiteral("Invalid large transcript reference."));
            return;
        }
        fetch(state);
        return;
    }
    state->page.insert(QStringLiteral("events"), state->events);
    // Partial items whose transfer is resolved here are no longer partial.
    QJsonArray retained;
    for (const QJsonValue& value : state->page.value(QLatin1String("partial_items")).toArray())
        if (value.toObject().value(QLatin1String("transfer_id")).toString().isEmpty())
            retained.append(value);
    if (state->page.contains(QLatin1String("partial_items")))
        state->page.insert(QStringLiteral("partial_items"), retained);
    finish(state, state->page, {});
}

} // namespace

void resolveLargeEvents(MachineBackend* backend, const QString& sessionId, const qint64 generation,
                        const QJsonObject& page, MachineBackend::Callback done) {
    auto state = std::make_shared<Resolution>();
    state->backend = backend;
    state->sessionId = sessionId;
    state->generation = generation;
    state->page = page;
    state->events = page.value(QLatin1String("events")).toArray();
    state->done = std::move(done);
    advance(state);
}

} // namespace ketplus::remote
