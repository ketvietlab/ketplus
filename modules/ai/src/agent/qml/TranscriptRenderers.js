.pragma library

// Rendering families are deliberately independent of provider tool names. Icons are Lucide
// names the kit maps (KetTheme.iconForLucide), chosen as mockups/qt's step kinds draw them.
const families = {
    command: { icon: "square-terminal", family: "command" },
    fileChange: { icon: "pencil", family: "files" },
    fileRead: { icon: "file-text", family: "files" },
    webSearch: { icon: "globe", family: "search" },
    codeSearch: { icon: "search", family: "search" },
    fileSearch: { icon: "search", family: "search" },
    ketplusBrowser: { icon: "globe", family: "browser" },
    browserOpen: { icon: "globe", family: "browser" },
    browserNavigate: { icon: "globe", family: "browser" },
    browserClick: { icon: "globe", family: "browser" },
    browserDownload: { icon: "globe", family: "browser" },
    browserScreenshot: { icon: "globe", family: "browser" },
    imageView: { icon: "image", family: "images" },
    imageGeneration: { icon: "sparkles", family: "images" },
    reasoning: { icon: "sparkles", family: "control" },
    thought: { icon: "sparkles", family: "control" },
    plan: { icon: "map", family: "control" },
    wait: { icon: "clock", family: "control" },
    monitor: { icon: "clock", family: "control" },
    wake: { icon: "clock", family: "control" },
    skill: { icon: "layers", family: "control" },
    toolsLoaded: { icon: "sliders-horizontal", family: "control" },
    taskStopped: { icon: "square-terminal", family: "control" },
    collaboration: { icon: "send", family: "control" },
    subAgent: { icon: "bot", family: "control" },
    webOpen: { icon: "globe", family: "integration" },
    webFind: { icon: "globe", family: "integration" },
    webFetch: { icon: "globe", family: "integration" },
    integration: { icon: "package", family: "integration" }
}

// A file step draws what happened to its file, as the mock's edit, create, delete and move.
const fileIcons = { Edited: "pencil", Created: "file-code", Deleted: "trash-2", Moved: "arrow-right" }

function renderer(subtype) {
    return families[subtype] || { icon: "sliders-horizontal", family: "generic" }
}

function icon(row) {
    if (row.subtype === "fileChange" && row.files.length === 1)
        return fileIcons[row.files[0].operation] || "pencil"
    // A listing is a file search the formatter titles "Listed files"; the mock draws it as a list.
    if (row.subtype === "fileSearch" && /^List(ed|ing) files\b/.test(row.title))
        return "list"
    return renderer(row.subtype).icon
}

function resource(row) {
    const value = row.target || row.payload.url || row.payload.path || ""
    if (/^https?:\/\//i.test(value)) return value
    if (/^[a-z][a-z0-9+.-]*:/i.test(value) && !/^file:\/\//i.test(value)) return ""
    return row.subtype === "fileRead" || row.subtype === "imageView"
        || row.subtype === "imageGeneration" || row.payload.path ? value : ""
}

function browserError(row) {
    const payload = row.payload || {}
    const result = payload.result || {}
    let message = (payload.error || {}).message || (result.structuredContent || {}).message || ""
    if (!message) {
        for (const block of result.content || []) {
            if (block.type !== "text") continue
            try { message = JSON.parse(block.text).message || "" } catch (_) { /* No raw tool output. */ }
            if (message) break
        }
    }
    return String(message || (row.status === "interrupted" ? "Browser action stopped." : "Browser action failed.")).slice(0, 320)
}

function detail(row) {
    if (row.subtype === "ketplusBrowser")
        return row.status === "failed" || row.status === "interrupted" ? browserError(row) : row.target || ""

    const family = renderer(row.subtype).family
    const rows = []
    // Semantic shell reads/searches retain their original command in details.
    if (row.command) {
        // Command rows render their input separately, with a one-line preview.
        // Keeping it here would repeat the entire input above the output.
        if (row.subtype !== "command") rows.push(row.command)
        if (row.cwd) rows.push("cwd: " + row.cwd)
        for (const action of row.details.actions || []) {
            if (action.type === "read") rows.push("Read: " + (action.path || action.name || ""))
            else if (action.type === "search") rows.push("Searched code: " + (action.query || "") + (action.path ? " in " + action.path : ""))
            else if (action.type === "listFiles") rows.push("Listed files: " + (action.path || ""))
        }
        if (row.exitCode !== undefined && row.exitCode !== null && row.exitCode !== "")
            rows.push("exit: " + row.exitCode)
        if (row.durationMs !== undefined && row.durationMs !== null && row.durationMs !== "")
            rows.push("duration: " + row.durationMs + " ms")
        // Output is rendered by TranscriptActivityRow in a bounded scrollable terminal frame.
    } else {
        if (row.target) rows.push(row.target)
        if (row.summary && row.summary !== row.target) rows.push(row.summary)
        if (family === "search" || row.subtype === "fileRead") {
            const result = row.payload.result || row.details.results
            if (result) rows.push(typeof result === "string" ? result : JSON.stringify(result, null, 2))
        }
        if (family === "generic" || family === "integration" || family === "control" || family === "browser") {
            if (Object.keys(row.payload).length) rows.push(JSON.stringify(row.payload, null, 2))
        }
    }
    if (row.status === "failed" || row.status === "interrupted") rows.unshift(row.status)
    return rows.join("\n")
}
