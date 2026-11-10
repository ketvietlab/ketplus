// SPDX-License-Identifier: MPL-2.0
import QtQuick

// In-memory mock ownership. Each conversation has its own model, timer and evidence.
QtObject {
    id: store
    property var plans: ({})
    property Component factory: Component { PlanRunModel {} }
    function get(conversationId) {
        if (!plans[conversationId]) {
            const model = factory.createObject(store, { conversationId: conversationId })
            const next = Object.assign({}, plans)
            next[conversationId] = model
            plans = next
        }
        return plans[conversationId]
    }
}
