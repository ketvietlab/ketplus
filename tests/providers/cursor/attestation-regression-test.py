#!/usr/bin/env python3
"""Offline regressions for observation order, readiness and safe diagnostics."""
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from bounded_diagnostics import Diagnostics
from private_attestation import Attestation

HERE = pathlib.Path(__file__).resolve().parent
STAGES = ["spawn", "initialize_sent", "initialize_result", "initialized_sent", "inventory"]
TERMINAL = ["exit", "close", "child_error", "stdout_eof", "stdout_close",
            "stdout_error", "stdin_close", "stdin_error"]


def accept(observer, event, sequence, **fields):
    observer.accept(dict(nonce=observer.nonce, transportEpoch=observer.epoch,
                         launchDigest=observer.digest, observerPid=observer.native_pid, childPid=456,
                         mainThread=True, injected=True, role="index.js", event=event,
                         sequence=sequence, **fields), observer.native_pid)


class AttestationRegressionTest(unittest.TestCase):
    def observer(self):
        observer = Attestation(dict(command=sys.executable, args=["fixture"], env=[]), str(HERE), "fixture")
        observer.native_pid = 123
        self.addCleanup(observer.close)
        return observer

    def test_socket_delivery_cannot_repair_invalid_observation_order_or_mutate_records(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node required for mock preload regression")
        result = subprocess.run([node, str(HERE / "attestation-order-fixture.cjs")],
                                capture_output=True, text=True, check=True, timeout=5)
        fixtures = json.loads(result.stdout)
        for case, expected in [("invalid", False), ("valid", True)]:
            with self.subTest(case=case):
                observer = self.observer()
                for row in fixtures[case]:
                    accept(observer, row["event"], row["sequence"])
                self.assertEqual(observer.snapshot()["ready"], expected)
        self.assertEqual([row["sequence"] for row in fixtures["invalid"]], [1, 3, 2, 4])
        row = fixtures["immutable"]
        self.assertEqual((row["event"], row["sequence"], row["observerPid"]), ("spawn", 1, 123))
        self.assertEqual(row["nested"], dict(value="observed"))
        self.assertEqual(fixtures["lifecycle"], ["spawn", *TERMINAL])
        self.assertNotIn("must not be logged", result.stdout)
        self.assertNotIn("private", result.stdout)

    def test_exact_worker_terminal_event_revokes_readiness_without_erasing_history(self):
        for event in TERMINAL:
            for delayed_handshake in [False, True]:
                with self.subTest(event=event, delayed_handshake=delayed_handshake):
                    observer = self.observer()
                    if delayed_handshake:
                        accept(observer, event, 6)
                    for sequence, stage in enumerate(STAGES):
                        accept(observer, stage, sequence, valid=True)
                    if not delayed_handshake:
                        self.assertTrue(observer.snapshot()["ready"])
                        accept(observer, event, 6)
                    state = observer.snapshot()
                    self.assertTrue(state["attested"])
                    self.assertTrue(state["inventory"])
                    self.assertTrue(state["revoked"])
                    self.assertFalse(state["ready"])

    def test_other_instance_exit_does_not_revoke_and_duplicate_stage_cannot_repair_handshake(self):
        observer = self.observer()
        for sequence, event in enumerate(STAGES):
            accept(observer, event, sequence, valid=True)
        observer.accept(dict(nonce=observer.nonce, transportEpoch=observer.epoch, launchDigest=observer.digest,
                             observerPid=123, role="index.js", mainThread=True, injected=True,
                             childPid=789, event="exit", sequence=6), 123)
        self.assertTrue(observer.snapshot()["ready"])
        accept(observer, "initialize_sent", 7)
        self.assertFalse(observer.snapshot()["ready"])

    def test_dead_injected_worker_blocks_prompt_without_reserving_or_forwarding(self):
        for terminal in ["exit", "stdout_eof", "child_error"]:
            for when in ["before_wait", "after_wait", "final_barrier"]:
                with self.subTest(terminal=terminal, when=when):
                    self.dead_worker_barrier(terminal, when)

    def dead_worker_barrier(self, terminal, when):
        spec = importlib.util.spec_from_file_location("bounded", HERE / "adapter-bounded-fixture.py")
        bounded = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bounded)
        observer = self.observer()
        original_snapshot = observer.snapshot
        observed = False
        snapshots = 0

        def snapshot():
            nonlocal observed, snapshots
            snapshots += 1
            if not observed:
                # main() has assigned the actual owned fake-provider PID.
                observed = True
                for sequence, event in enumerate(STAGES):
                    accept(observer, event, sequence, valid=True)
            if snapshots == {"before_wait": 1, "after_wait": 2, "final_barrier": 3}[when]:
                accept(observer, terminal, 6)
            return original_snapshot()

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            ledger = root / "mock-budget.json"
            ledger.write_text(json.dumps(dict(cursor=dict(sent_or_uncertain=0))))
            provider = root / "fake-provider"
            received = root / "received.jsonl"
            provider.write_text("#!" + sys.executable + "\n" + '''import json,sys
for line in sys.stdin:
 with open("received.jsonl", "a") as output: output.write(line)
 request=json.loads(line)
 print(json.dumps(dict(jsonrpc="2.0",id=request["id"],result=dict(configOptions=[dict(id="model",currentValue="claude-haiku-5-5")]))),flush=True)
''')
            provider.chmod(0o700)
            log = root / "events.jsonl"
            config = root / "launch.json"
            config.write_text(json.dumps(dict(executable=str(provider), work=str(root), log=str(log),
                                             mode="D-3", ledger=str(ledger), attestation=True)))

            def client():
                yield json.dumps(dict(id=1, method="session/set_config_option", params=dict(configId="model")))
                deadline = time.monotonic() + 3
                while not log.exists() or not any(row.get("event") == "cheap_model_ack" for row in bounded.rows(log)):
                    self.assertLess(time.monotonic(), deadline, "Fake model ACK timed out")
                    time.sleep(.01)
                yield json.dumps(dict(id=2, method="session/prompt", params=dict(
                    sessionId="synthetic", prompt=[dict(type="text", text=bounded.MARKER)])))

            output = io.StringIO()
            # Internal dependency patch only; no public bypass of module pinning.
            # No provider executable, real ledger, approval or auth file is used.
            with mock.patch.object(bounded, "make_attestation", return_value=observer), \
                    mock.patch.object(observer, "environment", return_value=dict(os.environ)), \
                    mock.patch.object(observer, "snapshot", side_effect=snapshot), \
                    mock.patch.object(bounded.signal, "signal"), \
                    mock.patch.object(sys, "argv", ["bounded", str(config), "acp"]), \
                    mock.patch.object(sys, "stdin", client()), mock.patch.object(sys, "stdout", output):
                bounded.main()
            events = bounded.rows(log)
            self.assertTrue(any(row["event"] == "cheap_model_ack" and row["accepted"] for row in events))
            self.assertTrue(any(row["event"] == "injected_attestation_final" and row["attested"] and
                                row["revoked"] and not row["ready"] for row in events))
            self.assertTrue(any(row["event"] == "prompt_barrier" and not row["mcp_ready"] for row in events))
            self.assertFalse(any(row["event"] == "model_request_forwarded" for row in events))
            self.assertEqual(json.loads(ledger.read_text())["cursor"]["sent_or_uncertain"], 0)
            self.assertEqual([json.loads(line)["method"] for line in received.read_text().splitlines()],
                             ["session/set_config_option"])
            self.assertIn("error", next(json.loads(line) for line in output.getvalue().splitlines()
                                        if json.loads(line).get("id") == 2))


class DiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.rows = []
        self.now = 0
        self.diagnostics = Diagnostics(lambda event, **fields: self.rows.append(dict(event=event, **fields)),
                                       clock=lambda: self.now, timeout=3)

    def test_rpc_error_is_numeric_and_untrusted_fields_never_escape(self):
        for code, expected in [(-32602, -32602), (True, None), ("secret", None), (2 ** 40, None)]:
            self.diagnostics.sent(1, "session/load")
            self.diagnostics.observe(dict(id=1, error=dict(code=code, message="private-token synthetic-transcript",
                                                         data=dict(secret="private-token"))))
            self.assertEqual(self.rows[-1], dict(event="native_diagnostic", phase="load", reason="rpc_error",
                                                 error_code=expected, exit_status=None))
        self.assertNotIn("private", json.dumps(self.rows))
        self.assertNotIn("secret", json.dumps(self.rows))

    def test_exit_eof_read_error_and_timeout_are_distinct_without_retry(self):
        for status, error, expected in [(17, False, "process_exit"), (-9, False, "process_exit"),
                                        (None, False, "stdout_eof"), (None, True, "read_error")]:
            self.diagnostics.sent(1, "session/prompt")
            self.diagnostics.closed(status, read_error=error)
            self.assertEqual(self.rows[-1]["reason"], expected)
            self.assertEqual(self.rows[-1]["phase"], "prompt")
            self.assertEqual(self.rows[-1]["exit_status"], status)
        self.diagnostics.sent(2, "session/prompt")
        self.now = 2.9
        self.diagnostics.expire()
        count = len(self.rows)
        self.now = 3
        self.diagnostics.expire()
        self.assertEqual(len(self.rows), count + 1)
        self.assertEqual(self.rows[-1]["reason"], "timeout")
        self.diagnostics.expire()
        self.diagnostics.closed(1)
        self.diagnostics.observe(dict(id=2, error=dict(code=-1)))
        self.assertEqual(len(self.rows), count + 1)

    def test_explicit_auto_control_timeout_does_not_record_raw_control_output(self):
        self.diagnostics.sent("auto", "session/prompt", auto_control=True)
        self.diagnostics.timed_out("auto")
        self.diagnostics.timed_out("auto")
        self.assertEqual(self.rows, [dict(event="native_diagnostic", phase="auto_control", reason="timeout",
                                          error_code=None, exit_status=None)])

    def test_assistant_plan_usage_patterns_are_allowlisted_observations_only(self):
        self.diagnostics.sent(1, "session/prompt")
        for text in ["private-token: not included in your ", "plan; usage ", "limit. synthetic-transcript"]:
            self.diagnostics.observe(dict(method="session/update", params=dict(update=dict(
                sessionUpdate="agent_message_chunk", content=dict(text=text)))))
        self.assertEqual([row["reason"] for row in self.rows], ["assistant_plan_limit", "assistant_usage_limit"])
        self.assertNotIn("private", json.dumps(self.rows))
        self.assertNotIn("synthetic", json.dumps(self.rows))
        self.diagnostics.observe(dict(id=1, result=dict(stopReason="end_turn")))
        self.assertEqual(self.rows[-1]["reason"], "response")


class WrapperDiagnosticsTest(unittest.TestCase):
    def test_mock_rpc_exit_timeout_and_assistant_limit_paths_emit_bounded_diagnostics(self):
        for scenario in ["rpc", "exit", "timeout", "assistant"]:
            with self.subTest(scenario=scenario):
                self.wrapper_diagnostics(scenario)

    def wrapper_diagnostics(self, scenario):
        spec = importlib.util.spec_from_file_location("bounded", HERE / "adapter-bounded-fixture.py")
        bounded = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bounded)
        expected = dict(rpc="rpc_error", exit="process_exit", timeout="timeout", assistant="assistant_usage_limit")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            provider = root / "fake-provider"
            provider.write_text("#!" + sys.executable + "\nSCENARIO=" + repr(scenario) + "\n" + '''import json,os,sys,time
for line in sys.stdin:
 request=json.loads(line)
 result=dict(configOptions=[dict(id="model",currentValue="claude-haiku-5-5")])
 response=dict(jsonrpc="2.0",id=request["id"],result=result)
 if request["method"]=="session/prompt":
  if SCENARIO=="rpc": response=dict(jsonrpc="2.0",id=request["id"],error=dict(code=-32042,message="private-token synthetic-transcript"))
  elif SCENARIO=="exit": os._exit(17)
  elif SCENARIO=="timeout": time.sleep(30)
  else:
   print(json.dumps(dict(jsonrpc="2.0",method="session/update",params=dict(update=dict(sessionUpdate="agent_message_chunk",content=dict(text="private-token: not available on your plan; usage limit; synthetic-transcript"))))),flush=True)
   response["result"]=dict(stopReason="end_turn")
 print(json.dumps(response),flush=True)
''')
            provider.chmod(0o700)
            ledger = root / "mock-budget.json"
            ledger.write_text(json.dumps(dict(decision="D-3", maximum_per_provider=1, owner_conversation=1,
                                             recall_allowed=False, retry_allowed=False,
                                             cursor=dict(sent_or_uncertain=0))))
            log = root / "events.jsonl"
            log.write_text(json.dumps(dict(event="tools_list", epoch="one", pid=0)) + "\n")
            config = root / "launch.json"
            config.write_text(json.dumps(dict(executable=str(provider), work=str(root), log=str(log),
                                             mode="D-3", ledger=str(ledger), head="mock-no-provider", owner=1)))

            def wait_until(predicate):
                deadline = time.monotonic() + 3
                while not predicate(bounded.rows(log)):
                    self.assertLess(time.monotonic(), deadline, "Mock diagnostic timed out")
                    time.sleep(.01)

            def client():
                yield json.dumps(dict(id=1, method="session/set_config_option", params=dict(configId="model")))
                wait_until(lambda rows: any(row["event"] == "cheap_model_ack" for row in rows))
                yield json.dumps(dict(id=2, method="session/prompt", params=dict(
                    sessionId="mock", prompt=[dict(type="text", text=bounded.MARKER)])))
                wait_until(lambda rows: any(row["event"] == "native_diagnostic" and
                                           row["phase"] == "prompt" and
                                           row["reason"] in [expected[scenario], "stdout_eof"] for row in rows))

            with mock.patch.object(bounded, "Diagnostics", side_effect=lambda record: Diagnostics(record, timeout=.3)), \
                    mock.patch.object(bounded.signal, "signal"), \
                    mock.patch.object(sys, "argv", ["bounded", str(config), "acp"]), \
                    mock.patch.object(sys, "stdin", client()), mock.patch.object(sys, "stdout", io.StringIO()):
                bounded.main()
            events = bounded.rows(log)
            diagnostics = [row for row in events if row["event"] == "native_diagnostic"]
            self.assertEqual(json.loads(ledger.read_text())["cursor"]["sent_or_uncertain"], 1)
            self.assertEqual(sum(row["event"] == "model_request_forwarded" for row in events), 1)
            if scenario == "exit":
                self.assertTrue(any(row["phase"] == "prompt" and row["reason"] in ["process_exit", "stdout_eof"]
                                    for row in diagnostics))
                self.assertTrue(any(row["reason"] == "process_exit" and row["exit_status"] == 17 for row in diagnostics))
            else:
                self.assertTrue(any(row["phase"] == "prompt" and row["reason"] == expected[scenario] for row in diagnostics))
            if scenario == "rpc":
                self.assertTrue(any(row["error_code"] == -32042 for row in diagnostics))
            if scenario == "assistant":
                self.assertTrue(any(row["reason"] == "assistant_plan_limit" for row in diagnostics))
            self.assertNotIn("private-token", log.read_text())
            self.assertNotIn("synthetic-transcript", log.read_text())
            for row in diagnostics:
                self.assertEqual(set(row), {"event", "pid", "epoch", "phase", "reason", "error_code", "exit_status"})


if __name__ == "__main__":
    unittest.main()
