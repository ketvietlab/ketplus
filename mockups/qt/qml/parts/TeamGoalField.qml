// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// The goal of New team (ADR 0114), as the app's TeamGoalEditor sets ket::TextArea up: six
// lines that grow to fourteen and then scroll, a 16,000-character count, and `@agent/`
// mentions of the conversations on this computer. A plain `@` completes to `@agent/`, since
// the goal mentions only conversations; a number no conversation has is drawn in the danger
// colours and named in `problem`, which the form shows before the team can start.
KetTextArea {
    id: goal

    readonly property int limit: 16000
    // Every numbered conversation of the mock's repositories.
    readonly property var conversations: {
        const all = []
        for (const repository of Fixtures.repositories)
            for (const conversation of repository.conversations || [])
                if (conversation.number)
                    all.push({ number: conversation.number, title: conversation.title, place: repository.name })
        return all
    }
    readonly property var unknown: {
        const known = conversations.map(c => c.number)
        const found = []
        for (const span of KetTheme.agentMentions(text))
            if (known.indexOf(span.number) < 0 && found.indexOf(span.number) < 0)
                found.push(span.number)
        return found
    }
    // The daemon also keeps the goal within 24 KiB of UTF-8, so it reaches the leader in one message.
    readonly property int byteLimit: 24 * 1024
    readonly property int bytes: {
        let total = 0
        for (const character of text) {
            const code = character.codePointAt(0)
            total += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4
        }
        return total
    }
    readonly property string problem: count > limit
        ? "The goal has " + count.toLocaleString(Qt.locale("en_US"), "f", 0) + " characters; a team keeps at most 16,000. Shorten it, or link to the details instead."
        : bytes > byteLimit
        ? "The goal is too long to reach the leader in one message: it takes " + bytes.toLocaleString(Qt.locale("en_US"), "f", 0) + " of 24,576 bytes, as accented letters, other scripts and emoji take more than one each. Shorten it, or link to the details instead."
        : unknown.length === 1 ? "@agent/" + unknown[0] + " is not a conversation on this computer. Correct the number or remove the mention."
        : unknown.length > 1 ? unknown.map(n => "@agent/" + n).join(", ") + " are not conversations on this computer. Correct the numbers or remove the mentions."
        : ""

    minimumLines: 6
    maximumLines: 14
    maxLength: limit
    invalid: unknown.length > 0
    unknownMentions: unknown
    placeholderText: "What should the team reach? Add the context, links and issues the leader needs, and type @ to mention a conversation the leader can ask."
    Accessible.name: "Goal"

    onMentionQuery: query => {
        const needle = query.trim().toLowerCase()
        mentionItems = conversations.filter(c => !needle || c.number.startsWith(needle)
                                                 || c.title.toLowerCase().indexOf(needle) >= 0).slice(0, 8)
    }
    onTextChanged: {
        for (const conversation of conversations)
            mentions.setTitle(conversation.number, conversation.title)
        // One typed `@` where a mention can start becomes `@agent/`.
        const at = cursorPosition - 1
        if (at >= 0 && text.charAt(at) === "@" && text.substr(at + 1, 6) !== "agent/"
                && (at === 0 || /[\s(\[{'"]/.test(text.charAt(at - 1)))
                && text.length === typedLength + 1) {
            typedLength = text.length + 6
            input.insert(at + 1, "agent/")
            return
        }
        typedLength = text.length
    }
    property int typedLength: 0
}
