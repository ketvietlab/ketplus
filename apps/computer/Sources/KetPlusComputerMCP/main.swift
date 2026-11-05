import ComputerProtocol
import Foundation

// Thin stdio MCP front for KetPlus Computer (ADR 0026). It never captures the
// screen or posts input itself; every tool call is forwarded to the agent app.

let serverVersion = "0.1.0"
let instructions = """
    KetPlus Computer controls native macOS apps. Use it only when no MCP server, shell command \
    or ketplus_browser tool can do the task. Call list_apps, then request_access for the apps \
    you need; the user approves them. Screenshots show only granted apps, and every coordinate \
    is a pixel of the most recent screenshot. Terminals and IDEs accept clicks only; browsers \
    other than Safari are view-only. If a tool reports CANCELLED, REMOTE_TURN or SECURE_INPUT, \
    stop and tell the user.
    """

// `--local-client <name>` serves an MCP client outside KetPlus, such as a terminal agent.
let localClient = CommandLine.arguments.firstIndex(of: "--local-client").map {
    $0 + 1 < CommandLine.arguments.count ? CommandLine.arguments[$0 + 1] : ""
}
let agent: AgentClient
do {
    agent = try AgentClient(localClient: localClient)
} catch {
    let failure = error as? AgentCallError
    FileHandle.standardError.write(Data("\(failure?.code ?? "ERROR"): \(failure?.message ?? "\(error)")\n".utf8))
    exit(2)
}

func emit(_ object: [String: Any]) {
    guard var data = try? JSONSerialization.data(withJSONObject: object) else { return }
    data.append(0x0A)
    FileHandle.standardOutput.write(data)
}

func text(_ value: String) -> [String: Any] { ["type": "text", "text": value] }

func imageResult(_ shot: [String: Any], _ note: String) -> [String: Any] {
    ["content": [["type": "image", "data": shot["png"] as? String ?? "", "mimeType": "image/png"], text(note)]]
}

func toolResult(_ name: String, _ arguments: [String: Any]) -> [String: Any] {
    do {
        switch name {
        case "wait":
            let seconds = min(max((arguments["duration"] as? NSNumber)?.doubleValue ?? 0, 0), ToolCatalog.maxWaitSeconds)
            Thread.sleep(forTimeInterval: seconds)
            return ["content": [text("Waited \(seconds) s.")]]
        case "screenshot":
            let shot = try agent.call("screenshot", arguments)
            let apps = (shot["visibleApps"] as? [String] ?? []).joined(separator: ", ")
            let display = shot["display"] as? [String: Any] ?? [:]
            let which = "display \(display["id"] ?? "?") \"\(display["name"] ?? "")\"\(display["work"] as? Bool == true ? " (work display)" : "")"
                + " at \(display["x"] ?? 0),\(display["y"] ?? 0) \(display["width"] ?? 0)x\(display["height"] ?? 0) points"
            var note = ""
            if let raised = shot["raised"] as? [String], !raised.isEmpty {
                note += " Raised hidden windows on the work display first: \(raised.joined(separator: ", "))."
            }
            if let covered = shot["covered"] as? [String], !covered.isEmpty {
                note += " Covered by other windows, so these may show old pixels: \(covered.joined(separator: ", "))."
                    + " Trust observe for their current state, or use open_application with foreground."
            }
            return imageResult(shot, "Screenshot \(shot["width"] ?? 0)x\(shot["height"] ?? 0) of \(which); granted apps visible: \(apps.isEmpty ? "none" : apps). Coordinates use this image.\(note)")
        case "zoom":
            let shot = try agent.call("zoom", arguments)
            return imageResult(shot, "Zoomed region \(shot["width"] ?? 0)x\(shot["height"] ?? 0). Click coordinates still refer to the last screenshot.")
        case "observe":
            let result = try agent.call("observe", arguments)
            let truncated = result["truncated"] as? Bool == true ? " (truncated; raise max_nodes)" : ""
            return ["content": [text("Snapshot \(result["snapshotId"] ?? "") of \(result["app"] ?? ""): "
                + "\(result["nodes"] ?? 0) elements\(truncated). Frames are x,y,width x height in screenshot pixels.\n"
                + "\(result["tree"] ?? "")")]]
        case _ where ToolCatalog.names.contains(name):
            let result = try agent.call(name, arguments)
            if result["ok"] as? Bool == true, result.count == 1 { return ["content": [text("OK")]] }
            let data = try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
            return ["content": [text(String(decoding: data, as: UTF8.self))]]
        default:
            return ["isError": true, "content": [text("Unknown tool \(name).")]]
        }
    } catch let failure as AgentCallError {
        return ["isError": true, "content": [text("\(failure.code): \(failure.message)")]]
    } catch {
        return ["isError": true, "content": [text("AGENT_UNAVAILABLE: \(error)")]]
    }
}

func handle(_ message: [String: Any]) {
    // Notifications carry no id and need no reply.
    guard let id = message["id"] else { return }
    let method = message["method"] as? String ?? ""
    let params = message["params"] as? [String: Any] ?? [:]
    switch method {
    case "initialize":
        emit(["jsonrpc": "2.0", "id": id, "result": [
            "protocolVersion": params["protocolVersion"] as? String ?? "2025-06-18",
            "capabilities": ["tools": [String: Any]()],
            "serverInfo": ["name": "ketplus_computer", "version": serverVersion],
            "instructions": instructions,
        ]])
    case "ping":
        emit(["jsonrpc": "2.0", "id": id, "result": [String: Any]()])
    case "tools/list":
        emit(["jsonrpc": "2.0", "id": id, "result": ["tools": ToolCatalog.tools]])
    case "tools/call":
        let name = params["name"] as? String ?? ""
        let arguments = params["arguments"] as? [String: Any] ?? [:]
        emit(["jsonrpc": "2.0", "id": id, "result": toolResult(name, arguments)])
    default:
        emit(["jsonrpc": "2.0", "id": id, "error": ["code": -32601, "message": "Method not found: \(method)"]])
    }
}

if CommandLine.arguments.contains("--status") {
    do {
        let status = try agent.call("status")
        let data = try JSONSerialization.data(withJSONObject: status, options: [.prettyPrinted, .sortedKeys])
        print(String(decoding: data, as: UTF8.self))
        exit(0)
    } catch {
        FileHandle.standardError.write(Data("\(error)\n".utf8))
        exit(1)
    }
}

while let line = readLine(strippingNewline: true) {
    guard !line.isEmpty, let data = line.data(using: .utf8),
          let message = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else {
        emit(["jsonrpc": "2.0", "id": NSNull(), "error": ["code": -32700, "message": "Parse error"]])
        continue
    }
    handle(message)
}
