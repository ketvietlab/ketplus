// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// Connections: this Mac, paired phones and trusted Linux machines (through the KetPlus
// Relay, as the app; there is no SSH), and the account. Pairing and trust are
// deterministic prototype states. Signed
// out, it introduces connecting (parts/ConnectionsIntro) and opens the app's browser
// sign-in dialog.
Item {
    id: screen

    required property var window
    property var phones: [{ name: "iPhone 16", detail: "KetPlus App · Paired" }]
    property var machines: [{ name: "build-node-01", detail: "Linux · Trusted", via: "Relay" }]
    property bool historyOpen: false
    property bool linuxLinkChecked: false
    property bool pairClaimed: false
    // What paired devices did on this Mac (#86): the daemon's remote command audit, newest
    // first. Only requests that can change something are recorded; reads are not.
    readonly property var activity: screen.window.remoteActivity ? activityFixture : []
    readonly property var activityFixture: [
        { kind: "turn.start", device: "iPhone 16", target: "Fix login timeout", when: "2 min ago", result: "ok" },
        { kind: "session.create", device: "iPhone 16", target: "New worktree feature/offline-cache", when: "18 min ago", result: "ok" },
        { kind: "turn.interrupt", device: "iPhone 16", target: "Refactor sync engine", when: "1 hour ago", result: "ok" },
        { kind: "session.create", device: "iPhone 16", target: "New worktree experiment/parser", when: "Yesterday, 21:14", result: "worktree_quota_exceeded" },
        { kind: "session.configuration.apply", device: "iPad Air", target: "Release notes draft", when: "Mon, 09:02", result: "ok" },
        { kind: "turn.start", device: "Unknown device", target: "Fix login timeout", when: "Sun, 23:40", result: "invalid_encrypted_command" },
        { kind: "session.delete", device: "iPad Air", target: "Old spike", when: "28 Oct, 17:55", result: "ok" }
    ]
    // The device the activity dialog shows; empty for every device.
    property string activityDevice
    readonly property var activityShown: activityDevice === ""
        ? activity : activity.filter(entry => entry.device === activityDevice)

    // The sign-in dialog after Continue in browser, until the simulated browser returns.
    property bool signInWaiting: false

    function openSignIn() {
        screen.signInWaiting = false
        signInDialog.open()
    }

    // `--open=pair|linux|signin` opens a dialog for the side by side with the app.
    function openDialog(name) {
        if (name === "pair")
            pairDialog.open()
        else if (name === "linux")
            linuxDialog.open()
        else if (name === "signin")
            openSignIn()
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function activityTitle(kind) {
        return ({ "turn.start": "Sent a message",
                  "turn.interrupt": "Stopped a turn",
                  "session.create": "Started a conversation",
                  "session.delete": "Deleted a conversation",
                  "session.configuration.apply": "Changed conversation settings" })[kind]
            || "Sent a " + kind + " request"
    }
    function activityIcon(kind) {
        return ({ "turn.start": "send", "turn.interrupt": "stop-circle", "session.create": "branch",
                  "session.delete": "trash", "session.configuration.apply": "sliders" })[kind] || "terminal"
    }
    // A refusal says why on the detail line; a sender that did not verify is named as the
    // device it claimed, so it is marked as a danger.
    function activityOutcome(result) {
        if (result === "ok")
            return { text: "Done", tone: KetTheme.BadgeTone.Success, reason: "" }
        if (result === "worktree_quota_exceeded")
            return { text: "Refused", tone: KetTheme.BadgeTone.Warning, reason: "Worktree limit reached" }
        if (result === "invalid_encrypted_command" || result === "sender_key_unavailable")
            return { text: "Refused", tone: KetTheme.BadgeTone.Danger, reason: "Sender not verified" }
        return { text: "Refused", tone: KetTheme.BadgeTone.Warning, reason: result }
    }
    function openActivity(device) {
        screen.activityDevice = device
        activityDialog.open()
    }
    function closeActivity() { activityDialog.close() }
    // Gives a dialog row keyboard focus, as Tab does (--open=device-focus).
    function focusActivityRow(index) {
        activityList.positionViewAtIndex(index, ListView.Contain)
        const row = activityList.itemAtIndex(index)
        if (row) row.forceActiveFocus(Qt.TabFocusReason)
    }

    component SectionTitle: Item {
        property string title
        property string count
        width: parent.width
        height: screen.px(28)
        Text { anchors.verticalCenter: parent.verticalCenter; text: title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(17); font.weight: Font.Medium }
        Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: count; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
    }

    component DeviceCard: Rectangle {
        default property alias trailing: trailingRow.data
        property string iconName
        property string name
        property string detail
        property var badges: []
        width: parent.width
        height: screen.px(94)
        radius: screen.px(7)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
        Rectangle {
            x: screen.px(22)
            anchors.verticalCenter: parent.verticalCenter
            width: screen.px(40); height: width; radius: screen.px(7)
            color: KetTheme.t.panelBgSubtle
            border.color: KetTheme.t.panelBorder
            KetIcon { anchors.centerIn: parent; name: iconName; box: screen.px(18); color: KetTheme.t.textSecondary }
        }
        Column {
            x: screen.px(80)
            anchors.verticalCenter: parent.verticalCenter
            spacing: screen.px(6)
            Row {
                spacing: screen.px(8)
                Text { anchors.verticalCenter: parent.verticalCenter; text: name; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                Repeater { model: badges; KetBadge { required property var modelData; text: modelData.text; tone: modelData.tone } }
            }
            Text { text: detail; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
        }
        Row { id: trailingRow; anchors.right: parent.right; anchors.rightMargin: screen.px(22); anchors.verticalCenter: parent.verticalCenter; spacing: screen.px(8) }
    }

    // One audited request: what it was, which device sent it, on what, when, and how it ended.
    // Rows take keyboard focus (Tab, or the arrow keys in a list) and expose the whole entry to
    // screen readers; a focused or hovered row shows the full text when any of it is cut short.
    component ActivityRow: Item {
        id: activityRow
        required property var entry
        property bool last: false
        property bool showDevice: true
        property int rowIndex: -1
        readonly property var outcome: screen.activityOutcome(entry.result)
        readonly property string fullText: screen.activityTitle(entry.kind) + ", " + entry.when + ". "
            + outcome.text + (outcome.reason ? ": " + outcome.reason : "") + ". "
            + entry.device + ", " + entry.target + "."
        width: parent.width
        height: screen.px(56)
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: fullText
        Accessible.focusable: true
        Accessible.focused: activeFocus
        Keys.onDownPressed: event => {
            const next = nextItemInFocusChain(true)
            if (next) next.forceActiveFocus(Qt.TabFocusReason)
        }
        Keys.onUpPressed: event => {
            const previous = nextItemInFocusChain(false)
            if (previous) previous.forceActiveFocus(Qt.BacktabFocusReason)
        }
        // Rows below the visible part of a list scroll into view as they take focus.
        onActiveFocusChanged: if (activeFocus && ListView.view) ListView.view.positionViewAtIndex(rowIndex, ListView.Contain)
        Rectangle {
            anchors.fill: parent
            anchors.margins: screen.px(2)
            radius: screen.px(5)
            color: "transparent"
            border.width: screen.px(2)
            border.color: activityRow.activeFocus ? KetTheme.t.focusBorder : "transparent"
        }
        KetIcon {
            x: screen.px(18)
            anchors.verticalCenter: parent.verticalCenter
            name: screen.activityIcon(entry.kind)
            box: screen.px(16)
            color: KetTheme.t.textSecondary
        }
        Column {
            x: screen.px(50)
            width: outcomeBadge.x - x - screen.px(16)
            anchors.verticalCenter: parent.verticalCenter
            spacing: screen.px(4)
            // When stays on the title line, so a long target never cuts it off.
            Row {
                width: parent.width
                spacing: screen.px(8)
                Text { id: titleText; width: Math.min(implicitWidth, parent.width - whenText.width - parent.spacing); elide: Text.ElideRight; text: screen.activityTitle(entry.kind); color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                Text { id: whenText; anchors.baseline: titleText.baseline; text: entry.when; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
            }
            Text {
                id: detailText
                width: parent.width
                elide: Text.ElideRight
                text: (outcome.reason ? outcome.reason + " · " : "") + (showDevice ? entry.device + " · " : "")
                      + entry.target
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: screen.px(12)
            }
        }
        HoverHandler { id: rowHover }
        // The full detail when it is cut short.
        KetToolTip {
            id: rowToolTip
            // At once for keyboard focus, which has no pointer to rest on the row.
            delay: activityRow.activeFocus ? 0 : recipe.delayMs
            visible: (rowHover.hovered || activityRow.activeFocus) && (detailText.truncated || titleText.truncated)
            text: titleText.text + ", " + whenText.text + "\n" + detailText.text
        }
        KetBadge { id: outcomeBadge; anchors.right: parent.right; anchors.rightMargin: screen.px(18); anchors.verticalCenter: parent.verticalCenter; text: outcome.text; tone: outcome.tone }
        Rectangle { visible: !last; anchors.bottom: parent.bottom; x: screen.px(18); width: parent.width - 2 * x; height: 1; color: KetTheme.t.panelBorder }
    }

    // Signed out: what connecting gives, the pairing steps and the Sign in action.
    Basic.ScrollView {
        id: introScroll
        visible: !screen.window.signedIn
        anchors.fill: parent
        contentWidth: availableWidth
        // --scroll: start part of the way down, for review captures.
        Timer {
            running: Mock.initialScroll >= 0 && Mock.initialScreen === "connections"
            interval: 250
            onTriggered: introScroll.contentItem.contentY = Mock.initialScroll
                         * Math.max(0, introScroll.contentItem.contentHeight - introScroll.height)
        }

        ConnectionsIntro {
            x: Math.max(screen.px(42), (introScroll.availableWidth - width) / 2)
            y: screen.px(36)
            width: Math.min(introScroll.availableWidth - screen.px(84), screen.px(1080))
            bottomPadding: screen.px(40)
            window: screen.window
            onSignInRequested: screen.openSignIn()
        }
    }

    Basic.ScrollView {
        id: scroll
        visible: screen.window.signedIn
        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            x: Math.max(screen.px(42), (scroll.availableWidth - width) / 2)
            y: screen.px(38)
            width: Math.min(scroll.availableWidth - screen.px(84), screen.px(1000))
            spacing: screen.px(18)

            PageHeading {
                width: parent.width
                title: "Connections"
                description: "Phones that access this Mac and Linux machines this desktop can use."
                KetButton {
                    id: addButton
                    text: "Add"
                    iconName: "plus"
                    variant: KetTheme.ButtonVariant.Primary
                    onClicked: addMenu.popup(addButton, addButton.width - addMenu.width, addButton.height + screen.px(4))
                }
            }

            Item { width: 1; height: screen.px(28) }

            Row {
                spacing: screen.px(38)
                Rectangle {
                    width: screen.px(84); height: width; radius: screen.px(10)
                    color: KetTheme.t.accentSubtle
                    border.color: KetTheme.t.accentBorder
                    KetIcon { anchors.centerIn: parent; name: "display"; box: screen.px(40); color: KetTheme.t.accentText }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(10)
                    Text { text: "This Mac"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(22); font.weight: Font.Medium }
                    Text { text: "Ready for your connected devices"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                    KetBadge { text: "Connected"; tone: KetTheme.BadgeTone.Success }
                }
            }

            Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }

            SectionTitle { title: "Phones & tablets"; count: screen.phones.length + " paired" }
            KetEmptyState {
                visible: screen.phones.length === 0
                width: parent.width
                iconName: "smartphone"
                title: "No phone paired"
                message: "Pair the KetPlus App to follow and steer conversations from your phone."
                actionText: "Pair a phone"
                onActionTriggered: pairDialog.open()
            }
            Repeater {
                model: screen.phones
                DeviceCard {
                    required property var modelData
                    required property int index
                    iconName: "smartphone"
                    name: modelData.name
                    detail: modelData.detail
                    KetButton { text: "Details"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.openActivity(modelData.name) }
                    KetButton {
                        text: "Revoke access"
                        iconName: "wifi-off"
                        variant: KetTheme.ButtonVariant.GhostDanger
                        onClicked: {
                            screen.phones = screen.phones.filter((p, i) => i !== index)
                            screen.window.notify("Revoked " + modelData.name)
                        }
                    }
                }
            }

            SectionTitle { title: "Activity from paired devices"; count: "Last 90 days" }
            KetEmptyState {
                visible: screen.activity.length === 0
                width: parent.width
                iconName: "clock"
                title: "No activity yet"
                message: "Messages, new conversations and other changes a paired device makes on this Mac appear here."
            }
            Rectangle {
                visible: screen.activity.length > 0
                width: parent.width
                height: recentColumn.height
                radius: screen.px(7)
                color: KetTheme.t.panelBg
                border.color: KetTheme.t.panelBorder
                Column {
                    id: recentColumn
                    width: parent.width
                    Accessible.role: Accessible.List
                    Accessible.name: "Activity from paired devices"
                    Repeater {
                        model: screen.activity.slice(0, 5)
                        ActivityRow {
                            required property var modelData
                            required property int index
                            entry: modelData
                            rowIndex: index
                            last: index === Math.min(screen.activity.length, 5) - 1
                        }
                    }
                }
            }
            Item {
                visible: screen.activity.length > 0
                width: parent.width
                height: showAll.height
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(8)
                    KetIcon { name: "shield"; box: screen.px(13); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: "Recorded on this Mac as each request finishes. Reading files and transcripts is not listed."; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                }
                KetButton {
                    id: showAll
                    anchors.right: parent.right
                    visible: screen.activity.length > 5
                    text: "Show all"
                    variant: KetTheme.ButtonVariant.Ghost
                    controlSize: KetTheme.ControlSize.Small
                    onClicked: screen.openActivity("")
                }
            }

            SectionTitle { title: "Linux machines"; count: screen.machines.length + " trusted" }
            Repeater {
                model: screen.machines
                DeviceCard {
                    required property var modelData
                    required property int index
                    iconName: "terminal"
                    name: modelData.name
                    detail: modelData.detail
                    // As the app: trusted through the Relay, with no SSH details or Disconnect.
                    badges: [{ text: "Available", tone: KetTheme.BadgeTone.Success },
                             { text: modelData.via, tone: KetTheme.BadgeTone.Neutral }]
                }
            }
            Row {
                spacing: screen.px(8)
                KetIcon { name: "shield"; box: screen.px(13); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "Linux machines connect through an authenticated outbound relay. Compare the identity shown here with ketplusctl on Linux."; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
            }

            Item {
                width: parent.width
                height: screen.px(64)
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(12)
                    KetAvatar { name: "An Nguyen"; anchors.verticalCenter: parent.verticalCenter }
                    Column {
                        Text { text: "Demo account"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                        Text { text: "Personal workspace"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    }
                }
                KetButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: "Sign out"; iconName: "log-out"; variant: KetTheme.ButtonVariant.Ghost; onClicked: { screen.window.signInExpired = false; screen.window.signedIn = false } }
            }

            KetButton {
                text: (screen.historyOpen ? "▾  " : "▸  ") + "About your history"
                variant: KetTheme.ButtonVariant.Ghost
                controlSize: KetTheme.ControlSize.Small
                onClicked: screen.historyOpen = !screen.historyOpen
            }
            Text {
                visible: screen.historyOpen
                width: parent.width
                wrapMode: Text.Wrap
                text: "Conversations, transcripts and files stay on this Mac. A paired phone reads them through an end-to-end encrypted session; the relay sees only ciphertext."
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: screen.px(13)
                lineHeight: 1.4
            }
            Item { width: 1; height: screen.px(40) }
        }
    }

    KetMenu {
        id: addMenu
        KetMenuItem { text: "Pair a phone or tablet"; description: "Scan a code with the KetPlus App"; iconName: "smartphone"; onTriggered: pairDialog.open() }
        KetMenuItem { text: "Connect a Linux machine"; description: "Through the KetPlus Relay"; iconName: "terminal"; onTriggered: linuxDialog.open() }
    }

    // The app's pairing: scan the code, then compare the confirmation code on both screens
    // and approve. There is no typed pairing code.
    KetDialog {
        id: pairDialog
        title: screen.pairClaimed ? "Confirm device" : "Connect your phone"
        onOpened: screen.pairClaimed = false
        // The app's layout: one centred column, the code under the words.
        Column {
            id: pairCopy
            Layout.fillWidth: true
            spacing: screen.px(8)
            component PairText: Text {
                width: pairCopy.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: screen.px(13)
            }
            PairText { text: screen.pairClaimed ? "Is this your phone?" : "Take your workspace with you."; color: KetTheme.t.textMain; font.pixelSize: screen.px(17); font.weight: Font.DemiBold }
            PairText { text: screen.pairClaimed ? "Compare the code on both devices." : "In KetPlus on your phone, choose Scan QR code." }
            Item { width: 1; height: screen.px(12) }
            Grid {
                visible: !screen.pairClaimed
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 9
                Repeater {
                    model: 81
                    Rectangle {
                        required property int index
                        width: screen.px(19); height: width
                        color: ((index * 7919) % 13 < 6 || [0, 1, 2, 9, 18, 6, 7, 8, 17, 26, 54, 63, 72, 73, 74].includes(index)) ? KetTheme.t.textMain : KetTheme.t.panelBg
                    }
                }
            }
            PairText { visible: screen.pairClaimed; text: "482 913"; color: KetTheme.t.textMain; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(28) }
            PairText { visible: screen.pairClaimed; text: "Device     iPad Air" }
            Item { width: 1; height: screen.px(12) }
            PairText { visible: !screen.pairClaimed; text: "Code expires in 04:59"; color: KetTheme.t.textMuted }
            Item { width: 1; height: screen.px(12) }
            PairText { visible: !screen.pairClaimed; color: KetTheme.t.textMain; horizontalAlignment: Text.AlignLeft; leftPadding: screen.px(64); lineHeight: 1.3; text: "1.  Scan this code.\n2.  Compare the confirmation codes.\n3.  Approve this device on your Mac." }
            Item { width: 1; height: screen.px(12) }
            PairText { text: "Closing or declining cancels this pairing ticket."; color: KetTheme.t.textMuted }
        }
        actions: [
            KetButton { text: screen.pairClaimed ? "Decline" : "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: pairDialog.reject() },
            KetButton {
                text: screen.pairClaimed ? "Yes, connect this device" : "Simulate scan"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    if (!screen.pairClaimed) {
                        screen.pairClaimed = true
                        return
                    }
                    screen.phones = screen.phones.concat([{ name: "iPad Air", detail: "KetPlus App · Paired just now" }])
                    pairDialog.accept()
                    screen.window.notify("iPad Air paired")
                }
            }
        ]
    }

    KetDialog {
        id: activityDialog
        title: screen.activityDevice === "" ? "Activity from paired devices" : screen.activityDevice
        // KetDialog lays its body out with no spacing; one column keeps the gaps and the sizes.
        Column {
            width: activityDialog.availableWidth
            spacing: screen.px(12)
            Text {
                visible: screen.activityDevice !== ""
                text: "KetPlus App · Paired · Last seen 2 minutes ago"
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: screen.px(13)
            }
            KetEmptyState {
                visible: screen.activityShown.length === 0
                width: parent.width
                iconName: "clock"
                title: "No activity yet"
                message: "Nothing this device asked for has changed this Mac in the last 90 days."
            }
            // Six rows show at once; the rest scroll inside the panel.
            Rectangle {
                visible: screen.activityShown.length > 0
                width: parent.width
                height: activityList.height
                radius: screen.px(7)
                color: KetTheme.t.panelBg
                border.color: KetTheme.t.panelBorder
                ListView {
                    id: activityList
                    width: parent.width
                    Accessible.role: Accessible.List
                    Accessible.name: activityDialog.title
                    height: Math.min(screen.activityShown.length, 6) * screen.px(56)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: screen.activityShown
                    delegate: ActivityRow {
                        required property var modelData
                        required property int index
                        width: activityList.width
                        entry: modelData
                        rowIndex: index
                        showDevice: screen.activityDevice === ""
                        last: index === screen.activityShown.length - 1
                    }
                    Basic.ScrollBar.vertical: Basic.ScrollBar { policy: activityList.contentHeight > activityList.height ? Basic.ScrollBar.AsNeeded : Basic.ScrollBar.AlwaysOff }
                }
            }
        }
        actions: [
            KetButton {
                visible: screen.activityDevice !== ""
                text: "Revoke access"
                iconName: "wifi-off"
                variant: KetTheme.ButtonVariant.GhostDanger
                onClicked: {
                    const device = screen.activityDevice
                    screen.phones = screen.phones.filter(p => p.name !== device)
                    activityDialog.accept()
                    screen.window.notify("Revoked " + device)
                }
            },
            KetButton { text: "Close"; variant: KetTheme.ButtonVariant.Primary; onClicked: activityDialog.accept() }
        ]
    }

    // The app's Linux pairing: a one-time ketplus:// link from ketplusctl, the identity
    // compared on both machines, then the approval checked.
    KetDialog {
        id: linuxDialog
        title: "Connect a Linux machine"
        onOpened: { screen.linuxLinkChecked = false; identityMatch.checked = false }
        KetFieldRow {
            // The dialog body is a ColumnLayout: a width binding would let the row overflow.
            Layout.fillWidth: true
            label: "Pairing link"
            help: "Run ketplusctl pairing create on the machine and paste the ketplus:// link it shows."
            KetTextField { width: parent.width; echoMode: TextInput.Password; placeholderText: "ketplus://pair?ticket=…&secret=…" }
        }
        Text {
            visible: screen.linuxLinkChecked
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Compare the first six characters of claimDigest shown by ketplusctl pairing status: 7F3A9C"
            color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13)
        }
        KetCheckbox { id: identityMatch; visible: screen.linuxLinkChecked; text: "I checked the identity on both machines." }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: linuxDialog.reject() },
            KetButton { text: "Check link"; onClicked: screen.linuxLinkChecked = true },
            KetButton {
                text: "Check approval"
                variant: KetTheme.ButtonVariant.Primary
                enabled: identityMatch.checked
                onClicked: {
                    screen.machines = screen.machines.concat([{ name: "build-node-02", detail: "Linux · Trusted", via: "Relay" }])
                    linuxDialog.accept()
                    screen.window.notify("build-node-02 trusted")
                }
            }
        ]
    }

    // The app's sign-in dialog: the browser signs in with the KetSuite account, and the
    // mock's browser returns after a moment.
    KetDialog {
        id: signInDialog
        title: "Sign in"
        onClosed: { screen.signInWaiting = false; browserReturn.stop() }
        // The body is a ColumnLayout: its children fill the width through Layout.
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Continue in your browser to sign in with your KetSuite account. KetPlus never sees your password."
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(KetTheme.t.textMd, KetTheme.zoom)
            lineHeight: 1.4
        }
        Text {
            visible: screen.signInWaiting
            Layout.fillWidth: true
            Layout.topMargin: screen.px(12)
            wrapMode: Text.Wrap
            text: "Finish signing in in your browser, then return here."
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(KetTheme.t.textMd, KetTheme.zoom)
        }
        actions: [
            KetButton {
                text: screen.signInWaiting ? "Waiting for your browser…" : "Continue in browser"
                iconName: "log-in"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    if (screen.signInWaiting)
                        return
                    screen.signInWaiting = true
                    browserReturn.start()
                }
            }
        ]
    }

    Timer {
        id: browserReturn
        interval: 1600
        onTriggered: {
            screen.window.signInExpired = false
            screen.window.signedIn = true
            signInDialog.accept()
            screen.window.notify("Signed in as An Nguyen")
        }
    }

    Connections {
        target: screen.window
        function onScreenChanged() {
            if (screen.window.screen !== "connections")
                signInDialog.close()
        }
    }
}
