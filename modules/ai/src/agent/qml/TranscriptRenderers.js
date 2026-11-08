.pragma library

// Rendering families are deliberately independent of provider tool names.
const families = {
    command: { icon: "square-terminal", family: "command" },
    fileChange: { icon: "file-code", family: "files" },
    fileRead: { icon: "file-code", family: "files" },
    webSearch: { icon: "search", family: "search" },
    codeSearch: { icon: "search", family: "search" },
    fileSearch: { icon: "search", family: "search" },
    ketplusBrowser: { icon: "search", family: "browser" },
    browserOpen: { icon: "search", family: "browser" },
    browserNavigate: { icon: "search", family: "browser" },
    browserClick: { icon: "search", family: "browser" },
    browserDownload: { icon: "search", family: "browser" },
    browserScreenshot: { icon: "image", family: "browser" },
    imageView: { icon: "image", family: "images" },
    imageGeneration: { icon: "image", family: "images" },
    reasoning: { icon: "brain", family: "control" },
    plan: { icon: "list-checks", family: "control" },
    wait: { icon: "clock", family: "control" },
    monitor: { icon: "activity", family: "control" },
    wake: { icon: "play", family: "control" },
    skill: { icon: "sparkles", family: "control" },
    toolsLoaded: { icon: "wrench", family: "control" },
    taskStopped: { icon: "square", family: "control" },
    collaboration: { icon: "users", family: "control" },
    subAgent: { icon: "bot", family: "control" },
    webOpen: { icon: "globe", family: "integration" },
    webFind: { icon: "search", family: "integration" },
    webFetch: { icon: "download", family: "integration" },
    integration: { icon: "bot", family: "integration" }
}

function renderer(subtype) {
    return families[subtype] || { icon: "bot", family: "generic" }
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
