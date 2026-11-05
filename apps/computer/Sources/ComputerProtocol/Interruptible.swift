import Foundation

/// Thrown when the user stops computer use (Esc or Stop) while a multi-step input runs.
public struct InputInterrupted: Error, Equatable {
    public init() {}
}

/// Runs the steps of one input action, checking before each step whether the user stopped it.
///
/// Typing 4,000 characters takes seconds; Stop revokes the grants at once, so no step may start
/// after it. A step that has begun (a key already down) still completes, so no key stays pressed.
public enum Interruptible {
    public static func forEach<S: Sequence>(_ steps: S, stopped: () -> Bool,
                                            _ body: (S.Element) throws -> Void) throws {
        for step in steps {
            if stopped() { throw InputInterrupted() }
            try body(step)
        }
    }
}
