import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

// Conversation-wide actions offered by a right-click anywhere in the transcript.
Basic.Menu {
    id: menu
    objectName: "transcriptContextMenu"
    required property var host
    required property var tokens
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
    padding: Metrics.px(5, tokens)
    margins: 8
    font.family: tokens.uiFont
    font.pixelSize: tokens.reading
    background: Rectangle {
        implicitWidth: Metrics.px(200, menu.tokens)
        implicitHeight: Metrics.px(44, menu.tokens)
        color: menu.tokens.panelBackground || menu.tokens.surfaceRaised
        border.color: menu.tokens.borderStrong || menu.tokens.border
        border.width: 1
        radius: 7
    }

    component Entry: Basic.MenuItem {
        id: entry
        width: menu.availableWidth
        implicitHeight: Metrics.px(34, menu.tokens)
        leftPadding: Metrics.px(10, menu.tokens)
        rightPadding: Metrics.px(28, menu.tokens)
        contentItem: Text {
            text: entry.text
            color: !entry.enabled ? menu.tokens.textDisabled : entry.highlighted ? menu.tokens.accentActive : menu.tokens.textMain
            font: entry.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: entry.highlighted ? menu.tokens.accentSubtle : "transparent"
            radius: Metrics.px(5, menu.tokens)
        }
    }

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
    Basic.MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: menu.tokens.border
        }
    }
    Entry {
        id: forkConversationItem
        objectName: "forkTranscriptConversationMenuItem"
        text: "Fork this conversation"
        enabled: menu.conversation !== null && menu.conversation.forkable === true
        onTriggered: {
            const id = menu.conversation ? menu.conversation.id : "";
            Qt.callLater(function() { menu.host.requestForkConversation(id); });
        }
    }
}
