.pragma library

// Keep a visible text position as well as the message boundary: Markdown can
// reflow earlier table cells without moving the message delegate itself.
function capture(view) {
    if (!view)
        return {followTail: true, index: -1, key: "", offset: 0, position: 0}
    // Anchor a materialized row whose actual top is inside the viewport, not
    // the delegate clipped at the leading edge. indexAt() can briefly return a
    // stale estimated row while ListView is polishing mixed-height delegates,
    // so rank the bounded viewport/cache delegates by their mapped position.
    const probe = Math.max(24, Math.min(view.height - 24, view.height / 3))
    let item = null
    let bestDistance = Number.POSITIVE_INFINITY
    if (view.contentItem) {
        for (const candidate of view.contentItem.children) {
            // Collapsed group members have zero height and never own the reading position.
            if (candidate.index === undefined || candidate.messageKey === undefined ||
                !candidate.visible)
                continue
            const screenY = candidate.mapToItem(view, 0, 0).y
            if (screenY < 24 || screenY >= view.height - 24)
                continue
            const distance = Math.abs(screenY - probe)
            if (distance < bestDistance) {
                item = candidate
                bestDistance = distance
            }
        }
    }
    let index = item ? item.index : -1
    // A single giant row can span the viewport without beginning inside it.
    // Fall back to the probe only when there is no stable interior row.
    for (let radius = 0; radius < 48 && index < 0; ++radius) {
        index = view.indexAt(1, view.contentY + probe + radius)
        if (index < 0 && radius > 0)
            index = view.indexAt(1, view.contentY + probe - radius)
    }
    // Very short or transiently unlaid-out lists may not reach the interior
    // probe. Retain the legacy leading-edge fallback for those cases.
    for (let offset = 0; offset < 24 && index < 0; ++offset)
        index = view.indexAt(1, view.contentY + offset)
    if (!item)
        item = index >= 0 ? view.itemAtIndex(index) : null
    // Persist what the reader sees while bounded mutation compensation is
    // active, not ListView's still-converging raw contentY/item.y estimate.
    const displayedOffset = item ? -item.mapToItem(view, 0, 0).y : 0
    const result = {followTail: view.followTail, index: index,
                    key: index >= 0 ? view.messageModel.get(index).messageKey : "",
                    offset: displayedOffset,
                    position: view.contentY - view.originY}
    const text = item ? item.readingTextItem : null
    if (!view.followTail && text && text.visible && text.length > 0) {
        const top = text.mapFromItem(view, 0, 0).y
        if (top >= 0 && top < text.height) {
            const position = text.positionAt(1, top)
            if (position >= 0 && position < text.length) {
                const rect = text.positionToRectangle(position)
                result.text = {position: position, offset: top - rect.y,
                               sample: text.getText(position, Math.min(text.length, position + 24)),
                               length: text.length,
                               prefixCharacters: text.renderedPrefixCharacters || 0,
                               partial: text.renderedPartialBody === true,
                               mermaidModes: text.renderedMermaidModes || 0}
            }
        }
    }
    return result
}

function matchingTextPosition(text, saved) {
    const prefix = Math.max(0, (text.renderedPrefixCharacters || 0) - (saved.prefixCharacters || 0))
    const becameRich = saved.partial === true && text.renderedPartialBody === false
    let expected = saved.position
    if (prefix > 0 || becameRich) {
        // Literal partial bodies have an exact UTF-16 prefix delta. When the
        // complete source becomes Markdown, rendered offsets also change; use
        // the net text length only as a bounded search hint, never as identity.
        expected += becameRich ? text.length - saved.length : prefix
    }
    if (!becameRich && expected >= 0 && expected + saved.sample.length <= text.length
            && text.getText(expected, expected + saved.sample.length) === saved.sample)
        return expected
    // Switching a Mermaid block between source and diagram replaces a stretch of text without
    // moving the rest by a known amount. The line is found again by its own words, near where it was.
    const relocated = (text.renderedMermaidModes || 0) !== (saved.mermaidModes || 0)
    if (prefix <= 0 && !becameRich && !relocated) return -1

    // Never scan a giant message or the full conversation to repair an anchor.
    // Ambiguous repeated samples deliberately fall back to the row anchor.
    const radius = 8192
    const center = Math.max(0, Math.min(text.length, expected))
    const start = Math.max(0, center - radius)
    const end = Math.min(text.length, center + radius)
    const nearby = text.getText(start, end)
    const found = nearby.indexOf(saved.sample)
    if (found < 0 || nearby.indexOf(saved.sample, found + 1) >= 0) return -1
    return start + found
}

function contentY(item, anchor) {
    const text = item.readingTextItem
    const saved = anchor.text
    if (saved && saved.sample && text && text.visible) {
        const position = matchingTextPosition(text, saved)
        if (position >= 0) {
            const rect = text.positionToRectangle(position)
            return item.y + text.mapToItem(item, 0, rect.y).y + saved.offset
        }
    }
    // Non-text cards, removed text, and older saved anchors use the row offset.
    return item.y + anchor.offset
}

function restore(view, item, anchor) {
    // Assigning contentY can synchronously trigger another ListView extent
    // fixup while estimated delegate heights converge. A clean Release build
    // can need more than one correction before the frame sync, so use a small
    // bounded fixed-point loop instead of leaving a visible jump to callLater.
    for (let pass = 0; pass < 4; ++pass) {
        const target = contentY(item, anchor)
        if (Math.abs(view.contentY - target) <= 0.01)
            break
        view.contentY = target
    }
}
