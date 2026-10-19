/// MCP tool definitions. Member names and inputs follow computer_toolset_20260801
/// so a later API driver can reuse the agent; list_apps, request_access and
/// open_application are KetPlus additions for per-app grants.
enum ToolCatalog {
    static let maxWaitSeconds = 60.0

    private static func object(_ properties: [String: Any], required: [String] = []) -> [String: Any] {
        ["type": "object", "properties": properties, "required": required, "additionalProperties": false]
    }

    private static let coordinate: [String: Any] = [
        "type": "array", "items": ["type": "integer", "minimum": 0], "minItems": 2, "maxItems": 2,
        "description": "[x, y] in pixels of the most recent screenshot",
    ]
    private static let modifiers: [String: Any] = [
        "type": "string", "description": "Modifier keys held during the action: shift, ctrl, alt, super or combinations like ctrl+shift",
    ]

    private static func clickTool(_ name: String, _ what: String) -> [String: Any] {
        ["name": name, "description": "\(what) at coordinate, or at the cursor when omitted.",
         "inputSchema": object(["coordinate": coordinate, "text": modifiers])]
    }

    static let tools: [[String: Any]] = [
        ["name": "list_apps",
         "description": "List running apps with their tier (full, click_only, view_only, blocked) and whether this conversation may use them.",
         "inputSchema": object([:])],
        ["name": "request_access",
         "description": "Ask the user to let this conversation see and control apps. Screenshots only show granted apps; input only reaches them.",
         "inputSchema": object([
             "apps": ["type": "array", "items": ["type": "string"], "minItems": 1, "maxItems": 8,
                      "description": "App names or bundle IDs"],
             "reason": ["type": "string", "description": "One sentence shown to the user"],
         ], required: ["apps"])],
        ["name": "open_application", "description": "Open or bring a granted app to the front.",
         "inputSchema": object(["app": ["type": "string", "description": "App name or bundle ID"]], required: ["app"])],
        ["name": "screenshot",
         "description": "Capture the main display showing only granted apps. Its pixels are the coordinate space for every other tool.",
         "inputSchema": object([:])],
        ["name": "zoom",
         "description": "Capture a region of the last screenshot at higher resolution to read small text. Coordinates stay those of the last screenshot.",
         "inputSchema": object(["region": ["type": "array", "items": ["type": "integer"], "minItems": 4, "maxItems": 4,
                                           "description": "[x0, y0, x1, y1] in screenshot pixels"]], required: ["region"])],
        clickTool("left_click", "Left-click"),
        clickTool("right_click", "Right-click"),
        clickTool("middle_click", "Middle-click"),
        clickTool("double_click", "Double-click"),
        clickTool("triple_click", "Triple-click"),
        ["name": "left_click_drag", "description": "Press at start_coordinate, drag to coordinate and release.",
         "inputSchema": object(["start_coordinate": coordinate, "coordinate": coordinate, "text": modifiers],
                               required: ["start_coordinate", "coordinate"])],
        ["name": "mouse_move", "description": "Move the pointer without clicking, for example to hover.",
         "inputSchema": object(["coordinate": coordinate], required: ["coordinate"])],
        ["name": "scroll", "description": "Scroll at coordinate, or at the cursor when omitted.",
         "inputSchema": object([
             "scroll_direction": ["type": "string", "enum": ["up", "down", "left", "right"]],
             "scroll_amount": ["type": "integer", "minimum": 1, "maximum": 50, "description": "Wheel clicks"],
             "coordinate": coordinate, "text": modifiers,
         ], required: ["scroll_direction", "scroll_amount"])],
        ["name": "type", "description": "Type literal text into the focused field of the frontmost granted app.",
         "inputSchema": object(["text": ["type": "string"]], required: ["text"])],
        ["name": "key", "description": "Press a key or combination such as Return, Escape, ctrl+s or super+shift+Tab.",
         "inputSchema": object(["text": ["type": "string"],
                                "repeat": ["type": "integer", "minimum": 1, "maximum": 100]], required: ["text"])],
        ["name": "cursor_position", "description": "Report the pointer position in screenshot pixels.",
         "inputSchema": object([:])],
        ["name": "wait", "description": "Pause before the next action, up to 60 seconds.",
         "inputSchema": object(["duration": ["type": "number", "minimum": 0, "maximum": maxWaitSeconds]],
                               required: ["duration"])],
    ]

    static let names = Set(tools.compactMap { $0["name"] as? String })
}
