// swift-tools-version:6.0
import PackageDescription

// ADR 0026 spike: the KetPlus Computer agent app and its stdio MCP front.
let package = Package(
    name: "KetPlusComputer",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "KetPlusComputer", targets: ["KetPlusComputer"]),
        .executable(name: "ketplus-computer-mcp", targets: ["KetPlusComputerMCP"]),
    ],
    targets: [
        .target(name: "ComputerProtocol"),
        .executableTarget(name: "KetPlusComputer", dependencies: ["ComputerProtocol"]),
        .executableTarget(name: "KetPlusComputerMCP", dependencies: ["ComputerProtocol"]),
        .testTarget(name: "ComputerProtocolTests", dependencies: ["ComputerProtocol"]),
    ],
    swiftLanguageModes: [.v5]
)
