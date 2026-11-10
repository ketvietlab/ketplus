// SPDX-License-Identifier: MPL-2.0
import QtQuick

QtObject {
    id: store
    property var sessions: ({})
    property Component factory: Component { ContentTabsModel {} }
    function get(conversationId) {
        if (!sessions[conversationId]) {
            const next = Object.assign({}, sessions)
            next[conversationId] = factory.createObject(store)
            sessions = next
        }
        return sessions[conversationId]
    }
}
