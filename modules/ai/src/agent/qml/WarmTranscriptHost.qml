import QtQuick

Item {
    id: root

    required property Component transcriptFactory
    required property var initialModel
    property var cachedTranscripts: []
    property var activeTranscript: null

    signal transcriptActivated(var transcript)

    function cached(scope) {
        for (let entry of cachedTranscripts)
            if (entry.scope === scope)
                return entry.view
        return null
    }

    function activate(scope, model) {
        let view = activeTranscript
        const warm = view !== null && view.cacheScope === scope
        if (!view) {
            view = transcriptFactory.createObject(root, {
                cacheScope: scope,
                messageModel: model,
                visible: true,
                enabled: true
            })
        } else {
            view.messageModel = model
            view.cacheScope = scope
        }
        cachedTranscripts = [{scope: scope, view: view}]
        activeTranscript = view
        view.objectName = "agentTranscript"
        view.visible = true
        view.enabled = true
        transcriptActivated(view)
        return {view: view, warm: warm}
    }

    function release(scope) {
        // Data and viewport ownership live outside this host. Keeping a single
        // ListView avoids hidden delegates mirroring the active conversation.
    }

    Component.onCompleted: activate("", initialModel)
}
