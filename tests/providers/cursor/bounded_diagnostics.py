"""Metadata-only diagnostics for owned synthetic ACP fixture requests.

No provider message, request parameter, identifier or transcript is retained.
Assistant limit patterns are observations, not an attribution of RPC failures.
"""
import threading
import time


PHASES = {"initialize": "initialize", "session/new": "new", "session/load": "load",
          "session/set_config_option": "model", "session/prompt": "prompt"}
REASONS = {"response", "rpc_error", "process_exit", "stdout_eof", "read_error", "timeout",
           "assistant_plan_limit", "assistant_usage_limit"}
REFUSAL_RULES = {
    "P1": ("plan", "upgrade your plan"),
    "P2": ("model", "named models unavailable"),
    "P3": ("model", "free plans can only use auto"),
    "P4": ("plan", "switch to auto or upgrade"),
    "P5": ("plan", "not included in your plan"),
    "P6": ("plan", "not available on your plan"),
    "U1": ("quota", "usage limit"),
    "U2": ("quota", "quota exceeded"),
}


def numeric(value):
    return value if type(value) is int and -(2 ** 31) <= value < 2 ** 31 else None


class Diagnostics:
    def __init__(self, record, clock=time.monotonic, timeout=30):
        self.record = record
        self.clock = clock
        self.timeout = timeout
        self.pending = {}
        self.lock = threading.Lock()

    def report(self, phase, reason, code=None, status=None):
        assert phase in {*PHASES.values(), "auto_control", "transport", "other"}
        assert reason in REASONS
        self.record("native_diagnostic", phase=phase, reason=reason,
                    error_code=numeric(code), exit_status=numeric(status))

    def sent(self, serial, method, auto_control=False):
        with self.lock:
            self.pending[str(serial)] = dict(phase="auto_control" if auto_control else PHASES.get(method, "other"),
                                             deadline=self.clock() + self.timeout, tail="", limits=set(), rules=set())

    def observe(self, value):
        with self.lock:
            if "id" in value and "method" not in value:
                request = self.pending.pop(str(value["id"]), None)
                if request:
                    error = value.get("error")
                    self.report(request["phase"], "rpc_error" if "error" in value else "response",
                                code=error.get("code") if isinstance(error, dict) else None)
            if value.get("method") != "session/update":
                return
            update = value.get("params", {}).get("update", {})
            if update.get("sessionUpdate") != "agent_message_chunk":
                return
            text = update.get("content", {}).get("text", "")
            if not isinstance(text, str):
                return
            for request in self.pending.values():
                if request["phase"] != "prompt":
                    continue
                # Bounded rolling buffer handles split chunks. Only enum
                # matches escape this lock; raw text is never sent to record().
                lower = (request["tail"] + text).lower()
                for rule_id, (category, pattern) in REFUSAL_RULES.items():
                    if rule_id not in request["rules"] and pattern in lower:
                        request["rules"].add(rule_id)
                        self.record("assistant_refusal_rule", rule_id=rule_id, category=category)
                for reason, patterns in {
                    "assistant_plan_limit": ("not included in your plan", "not available on your plan"),
                    "assistant_usage_limit": ("usage limit", "quota exceeded"),
                }.items():
                    if reason not in request["limits"] and any(pattern in lower for pattern in patterns):
                        request["limits"].add(reason)
                        self.report("prompt", reason)
                request["tail"] = lower[-128:]

    def expire(self):
        with self.lock:
            for serial, request in list(self.pending.items()):
                if self.clock() >= request["deadline"]:
                    self.pending.pop(serial)
                    self.report(request["phase"], "timeout")

    def timed_out(self, serial):
        with self.lock:
            request = self.pending.pop(str(serial), None)
            if request:
                self.report(request["phase"], "timeout")

    def closed(self, status=None, read_error=False):
        with self.lock:
            reason = "read_error" if read_error else "process_exit" if numeric(status) is not None else "stdout_eof"
            for request in self.pending.values():
                self.report(request["phase"], reason, status=status)
            self.pending.clear()
