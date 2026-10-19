import ComputerProtocol
import Foundation

// Thin stdio MCP front for KetPlus Computer (ADR 0026). It never captures the
// screen or posts input itself; every tool call is forwarded to the agent app.

let serverVersion = "0.0.1-spike"
let instructions = """
    KetPlus Computer controls native macOS applications. Use it only when no MCP server, \
    shell command or ketplus_browser tool can do the task. Take a screenshot first; \
    click coordinates are pixels in the most recent screenshot. KetPlus windows are \
    excluded from screenshots.
    """

let tools: [[String: Any]] = [
    [
        "name": "screenshot",
        "description": "Capture the main display, downscaled. Returns a PNG whose pixels are the coordinate space for clicks.",
        "inputSchema": ["type": "object", "properties": [String: Any](), "additionalProperties": false],
    ],
    [
        "name": "left_click",
        "description": "Left-click at [x, y] pixels in the most recent screenshot.",
        "inputSchema": [
            "type": "object",
            "properties": [
                "coordinate": [
                    "type": "array", "items": ["type": "integer", "minimum": 0], "minItems": 2, "maxItems": 2,
                    "description": "[x, y] in screenshot pixels",
                ],
            ],
            "required": ["coordinate"],
            "additionalProperties": false,
        ],
    ],
]

let agent = AgentClient()

func emit(_ object: [String: Any]) {
    guard var data = try? JSONSerialization.data(withJSONObject: object) else { return }
    data.append(0x0A)
    FileHandle.standardOutput.write(data)
}

func toolResult(_ name: String, _ arguments: [String: Any]) -> [String: Any] {
    do {
        switch name {
        case "screenshot":
            let shot = try agent.call("screenshot")
            let width = shot["width"] as? Int ?? 0
            let height = shot["height"] as? Int ?? 0
            return ["content": [
                ["type": "image", "data": shot["png"] as? String ?? "", "mimeType": "image/png"],
                ["type": "text", "text": "Screenshot \(width)x\(height). Click coordinates use this image."],
            ]]
        case "left_click":
            _ = try agent.call("left_click", ["coordinate": arguments["coordinate"] ?? NSNull()])
            return ["content": [["type": "text", "text": "Clicked. Take a screenshot to verify the result."]]]
        default:
            return ["isError": true, "content": [["type": "text", "text": "Unknown tool \(name)."]]]
        }
    } catch let failure as AgentCallError {
        return ["isError": true, "content": [["type": "text", "text": "\(failure.code): \(failure.message)"]]]
    } catch {
        return ["isError": true, "content": [["type": "text", "text": "AGENT_UNAVAILABLE: \(error)"]]]
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
        emit(["jsonrpc": "2.0", "id": id, "result": ["tools": tools]])
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
