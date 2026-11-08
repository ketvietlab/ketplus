import QtQuick
import Ket as Kit

// Conversation-wide actions offered by a right-click anywhere in the transcript.
Kit.KetMenu {
    id: menu
    objectName: "transcriptContextMenu"
    required property var host

    // The text item holding the reader's selection when the menu opened, if any.
    property var selectionItem: null
    property string fileTarget: ""
    readonly property var conversation: {
        const conversations = host.conversations || [];
        for (let i = 0; i < conversations.length; ++i) {
            if (conversations[i].active)
                return conversations[i];
        }
        return null;
    }
    property alias forkItem: forkConversationItem
    property alias copyConversationItem: copyConversationItem
    signal copyConversationRequested
    component Entry: Kit.KetMenuItem {}

    Entry {
        objectName: "revealTranscriptFileMenuItem"
        text: menu.host.revealFileActionText()
        visible: menu.fileTarget.length > 0
        height: visible ? implicitHeight : 0
        onTriggered: menu.host.revealFile(menu.fileTarget)
    }
    Entry {
        objectName: "copyTranscriptSelectionMenuItem"
        text: "Copy"
        enabled: !!menu.selectionItem && menu.selectionItem.selectedText.length > 0
        onTriggered: menu.host.copySelectedText(menu.selectionItem.selectedText)
    }
    Entry {
        id: copyConversationItem
        objectName: "copyTranscriptConversationMenuItem"
        text: menu.host.historyState.copying ? "Cancel copying conversation" : menu.host.sessionRunning ? "Copy loaded conversation" : "Copy conversation"
        onTriggered: menu.copyConversationRequested()
    }
    Kit.KetMenuSeparator {}
    Entry {
        id: forkConversationItem
        objectName: "forkTranscriptConversationMenuItem"
        text: "Fork this conversation"
        enabled: menu.conversation !== null && menu.conversation.forkable === true
        onTriggered: {
            const id = menu.conversation ? menu.conversation.id : "";
            Qt.callLater(function () {
                menu.host.requestForkConversation(id);
            });
        }
    }
}
