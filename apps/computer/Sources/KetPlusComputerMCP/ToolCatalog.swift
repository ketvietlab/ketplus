/// MCP tool definitions. Member names and inputs follow computer_toolset_20260801
/// so a later API driver can reuse the agent; list_apps, request_access,
/// open_application, observe and act are KetPlus additions (ADR 0026, 0027).
/// Input runs in the background by default; `foreground` opts into the real pointer.
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

    private static let foreground: [String: Any] = [
        "type": "boolean",
        "description": "Use the real pointer or keyboard. It moves the user's cursor and needs the target on top; "
            + "use it only after NEEDS_FOREGROUND.",
    ]
    private static let app: [String: Any] = [
        "type": "string", "description": "App name or bundle ID to type into; defaults to the app last acted on",
    ]

    private static func clickTool(_ name: String, _ what: String, background: Bool) -> [String: Any] {
        ["name": name,
         "description": background
             ? "\(what) at coordinate in the background: the agent presses, focuses or selects the accessible control "
                 + "there, even under other windows, without moving the user's pointer."
             : "\(what) at coordinate with the real pointer (foreground only).",
         "inputSchema": object(["coordinate": coordinate, "text": modifiers, "foreground": foreground])]
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
        ["name": "open_application",
         "description": "Open a granted app. It stays behind the user's work unless foreground is true.",
         "inputSchema": object(["app": ["type": "string", "description": "App name or bundle ID"],
                                "foreground": ["type": "boolean", "description": "Bring the app to the front"]],
                               required: ["app"])],
        ["name": "observe",
         "description": "Read a granted app's accessibility tree, even behind other windows. Each element has a ref "
             + "for act, its actions and its frame in screenshot pixels. Any input makes the refs stale.",
         "inputSchema": object(["app": ["type": "string", "description": "App name or bundle ID"],
                                "max_nodes": ["type": "integer", "minimum": 20, "maximum": 1500]],
                               required: ["app"])],
        ["name": "act",
         "description": "Act on an element from the latest observe, in the background. Prefer this to clicking coordinates.",
         "inputSchema": object([
             "snapshot_id": ["type": "string"], "ref": ["type": "string", "description": "Element ref such as n12"],
             "action": ["type": "string", "enum": ["press", "focus", "select", "show_menu", "set_value", "increment",
                                                   "decrement", "confirm", "cancel", "raise", "pick", "scroll_to_visible"]],
             "value": ["type": "string", "description": "Text for set_value"],
         ], required: ["snapshot_id", "ref", "action"])],
        ["name": "screenshot",
         "description": "Capture the main display showing only granted apps. Its pixels are the coordinate space for every other tool.",
         "inputSchema": object([:])],
        ["name": "zoom",
         "description": "Capture a region of the last screenshot at higher resolution to read small text. Coordinates stay those of the last screenshot.",
         "inputSchema": object(["region": ["type": "array", "items": ["type": "integer"], "minItems": 4, "maxItems": 4,
                                           "description": "[x0, y0, x1, y1] in screenshot pixels"]], required: ["region"])],
        clickTool("left_click", "Left-click", background: true),
        clickTool("right_click", "Right-click (opens the control's menu in the background)", background: true),
        clickTool("middle_click", "Middle-click", background: false),
        clickTool("double_click", "Double-click", background: false),
        clickTool("triple_click", "Triple-click", background: false),
        ["name": "left_click_drag",
         "description": "Press at start_coordinate, drag to coordinate and release, with the real pointer (foreground only).",
         "inputSchema": object(["start_coordinate": coordinate, "coordinate": coordinate, "text": modifiers,
                                "foreground": foreground],
                               required: ["start_coordinate", "coordinate"])],
        ["name": "mouse_move", "description": "Move the real pointer without clicking, for example to hover (foreground only).",
         "inputSchema": object(["coordinate": coordinate, "foreground": foreground], required: ["coordinate"])],
        ["name": "scroll",
         "description": "Scroll the scroll area at coordinate. In the background it moves the area's scroll bar.",
         "inputSchema": object([
             "scroll_direction": ["type": "string", "enum": ["up", "down", "left", "right"]],
             "scroll_amount": ["type": "integer", "minimum": 1, "maximum": 50, "description": "Wheel clicks"],
             "coordinate": coordinate, "text": modifiers, "foreground": foreground,
         ], required: ["scroll_direction", "scroll_amount"])],
        ["name": "type",
         "description": "Type literal text into the focused field of a granted app, in the background.",
         "inputSchema": object(["text": ["type": "string"], "app": app, "foreground": foreground], required: ["text"])],
        ["name": "key",
         "description": "Press a key or combination such as Return, Escape, ctrl+s or super+shift+Tab, in the background.",
         "inputSchema": object(["text": ["type": "string"], "app": app, "foreground": foreground,
                                "repeat": ["type": "integer", "minimum": 1, "maximum": 100]], required: ["text"])],
        ["name": "cursor_position", "description": "Report the pointer position in screenshot pixels.",
         "inputSchema": object([:])],
        ["name": "wait", "description": "Pause before the next action, up to 60 seconds.",
         "inputSchema": object(["duration": ["type": "number", "minimum": 0, "maximum": maxWaitSeconds]],
                               required: ["duration"])],
    ]

    static let names = Set(tools.compactMap { $0["name"] as? String })
}
