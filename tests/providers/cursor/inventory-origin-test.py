"""Quota-free owned-history inventory fixtures. Never launches a provider CLI."""
import copy
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import time
import unittest
from unittest import mock

import cursor_extra_budget as budget_api

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("bounded_inventory", HERE / "adapter-bounded-fixture.py")
bounded = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bounded)


class InventoryOriginTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.session = "synthetic-origin-two"
        d3 = self.root / "d3.json"
        d3.write_text(json.dumps(dict(decision="D-3", owner_conversation=1,
            **{name: dict(sent_or_uncertain=1) for name in ["codex", "cursor", "copilot"]})))
        d7 = self.root / "d7.json"
        d7.write_text(json.dumps(dict(decision="D-7", owner_conversation=1,
            previous_ledger_sha256=hashlib.sha256(d3.read_bytes()).hexdigest(), cursor=dict(sent_or_uncertain=1))))
        self.parents = {p: p.read_bytes() for p in [d3, d7]}
        summary = self.root / "origin-summary.json"
        summary.write_text(json.dumps(dict(head="origin-head", ordinal=2, used=2, returncode=0,
            forwarded=1, history_marker=True, cleanup_confirmed=True,
            canonical=[dict(phase="adapter_turn", reason="completed")])) )
        self.budget = dict(scope="cursor-extra-10", maximum_total=10, maximum_per_provider=10,
            allowed_providers=["cursor"], owner_conversation=1, retry_allowed=False, recall_allowed=True,
            authorization=dict(kind="direct_user_instruction", reference="fixture criterion", conversation=1,
                               date="fixture-date", quote="synthetic authorization", relay="fixture-relay"),
            parent_ledgers=[dict(decision=name, path=str(path), sha256=hashlib.sha256(content).hexdigest())
                            for name, (path, content) in zip(["D-3", "D-7"], self.parents.items())],
            cursor=dict(sent_or_uncertain=2, session_id=self.session, synthetic_cwd=str(self.root)),
            invocations=[dict(ordinal=1, provider="cursor", purpose="canonical_failure_diagnostic"),
                         dict(ordinal=2, provider="cursor", purpose="completed_history_refresh", model="composer-2.5",
                              head="origin-head", status="completed_with_history_replay", history_verified=True,
                              cleanup_confirmed=True, session_id=self.session,
                              summary_sha256=hashlib.sha256(summary.read_bytes()).hexdigest())])
        self.config = dict(mode="cursor-extra-10", purpose="injected_epoch_inventory", model="composer-2.5",
            owner=1, head="inventory-head", authorization_reference="fixture criterion", attestation=True,
            work=str(self.root), inventory_resume=self.session, origin_summary=str(summary))

    def test_origin_is_bound_to_completed_second_invocation_and_summary_hash(self):
        self.assertEqual(budget_api.inventory_origin(self.config, self.budget), self.session)
        for fields in [dict(owner=2), dict(work="other"), dict(inventory_resume="other"),
                       dict(purpose="completed_history_refresh"), dict(model="auto"), dict(attestation=False)]:
            with self.subTest(fields=fields), self.assertRaises(AssertionError):
                budget_api.inventory_origin(dict(self.config, **fields), self.budget)
        for fields in [dict(status="reserved_before_delivery"), dict(history_verified=False),
                       dict(cleanup_confirmed=False), dict(model="composer-2.5-fast"),
                       dict(purpose="synthetic_recall"), dict(head="other-head"), dict(session_id="other")]:
            budget = copy.deepcopy(self.budget)
            budget["invocations"][1].update(fields)
            with self.subTest(fields=fields), self.assertRaises(AssertionError):
                budget_api.inventory_origin(self.config, budget)
        path = pathlib.Path(self.config["origin_summary"])
        path.write_bytes(path.read_bytes() + b" ")
        with self.assertRaises(AssertionError):
            budget_api.inventory_origin(self.config, self.budget)

    def test_reservation_requires_exact_id_and_cannot_be_reused(self):
        with self.assertRaises(AssertionError):
            budget_api.record_reservation(self.config, self.budget, "composer-2.5", "other")
        self.assertEqual(self.budget["cursor"]["sent_or_uncertain"], 2)
        budget_api.record_reservation(self.config, self.budget, "composer-2.5", self.session)
        self.assertEqual(self.budget["cursor"]["sent_or_uncertain"], 3)
        self.assertEqual(self.budget["invocations"][2]["purpose"], "injected_epoch_inventory")
        with self.assertRaises(AssertionError):
            budget_api.record_reservation(self.config, self.budget, "composer-2.5", self.session)

    def test_wrapper_loads_owned_history_then_reserves_once_and_rejects_fallbacks(self):
        self.run_wrapper("success", bad_loads=True)

    def test_no_model_control_does_not_reserve(self):
        self.run_wrapper("success", control=True)

    def test_load_failure_does_not_reserve_or_forward(self):
        self.run_wrapper("failure")

    def test_missing_history_does_not_reserve_or_forward(self):
        self.run_wrapper("missing")

    def test_wrong_history_session_does_not_reserve_or_forward(self):
        self.run_wrapper("wrong_history")

    def test_conflicting_load_echo_does_not_reserve_or_forward(self):
        self.run_wrapper("echo_mismatch")

    def run_wrapper(self, scenario, control=False, bad_loads=False):
        ledger = self.root / "budget.json"
        ledger.write_text(json.dumps(self.budget))
        provider = self.root / "fake-provider"
        provider.write_text("#!" + sys.executable + "\n" +
            "LEDGER=" + repr(str(ledger)) + "\nSCENARIO=" + repr(scenario) + "\n" + '''import json,sys
for line in sys.stdin:
 r=json.loads(line); method=r['method']; params=r['params']; result={}
 if method=='session/new': raise AssertionError('No newSession fallback')
 if method=='session/load':
  assert params['sessionId']=='synthetic-origin-two'
  if SCENARIO=='failure':
   print(json.dumps(dict(jsonrpc='2.0',id=r['id'],error=dict(code=-32602,message='mock load denied'))),flush=True)
   continue
  if SCENARIO!='missing':
   sid='different' if SCENARIO=='wrong_history' else params['sessionId']
   print(json.dumps(dict(jsonrpc='2.0',method='session/update',params=dict(sessionId=sid,
    update=dict(sessionUpdate='agent_message_chunk',content=dict(type='text',text='T3_BOUNDED_CURSOR_ALPHA'))))),flush=True)
  if SCENARIO=='echo_mismatch': result={'sessionId':'different'}
 if method=='session/set_config_option': result={'configOptions':[{'id':'model','currentValue':'composer-2.5'}]}
 if method=='session/prompt':
  assert params['sessionId']=='synthetic-origin-two'
  budget=json.load(open(LEDGER)); assert budget['cursor']['sent_or_uncertain']==3
  assert budget['invocations'][2]['purpose']=='injected_epoch_inventory'
  result={'stopReason':'end_turn'}
 print(json.dumps(dict(jsonrpc='2.0',id=r['id'],result=result)),flush=True)
''')
        provider.chmod(0o700)
        log = self.root / "events.jsonl"
        session = dict(cwd=str(self.root), mcpServers=[dict(name="synthetic", command="mock")])
        config = dict(self.config, mode="control" if control else "cursor-extra-10", executable=str(provider),
                      ledger=str(ledger), log=str(log), approved_session=session)
        path = self.root / "launch.json"
        path.write_text(json.dumps(config))

        class Attestation:
            events = []
            revoked = False
            def environment(self): return dict(os.environ)
            def snapshot(self):
                return dict(attested=True, ready=not self.revoked, revoked=self.revoked, transport_closed=self.revoked,
                            workers=["mock"], all_workers=[], inventory=True, rejected=0, forbidden=False)
            def revoke_transport(self): self.revoked = True
            def close(self): pass

        def wait_for(event, count=1):
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                if sum(row["event"] == event for row in bounded.rows(log)) >= count:
                    return
                time.sleep(.01)
            self.fail("Mock event timed out: " + event)

        def request(serial, method, params):
            return json.dumps(dict(jsonrpc="2.0", id=serial, method=method, params=params))

        def client():
            yield request(1, "initialize", {})
            if bad_loads:
                yield request(2, "session/new", session)
                wait_for("inventory_origin_rejected")
                yield request(3, "session/load", dict(session, sessionId="other"))
                wait_for("inventory_origin_rejected", 2)
            yield request(4, "session/load", dict(session, sessionId=self.session))
            wait_for("load_ack")
            yield request(5, "session/set_config_option", dict(configId="model", value="composer-2.5[fast=false]"))
            wait_for("cheap_model_ack")
            prompt = dict(sessionId=self.session, prompt=[dict(type="text", text=bounded.MARKER)])
            yield request(6, "session/prompt", prompt)
            if scenario != "success":
                wait_for("inventory_prompt_rejected")
            elif control:
                wait_for("prompt_intercepted")
            else:
                wait_for("native_prompt_response")
                yield request(7, "session/prompt", prompt)
                wait_for("inventory_prompt_rejected")

        with mock.patch.object(bounded, "make_attestation", return_value=Attestation()), \
                mock.patch.object(bounded.signal, "signal"), mock.patch.object(sys, "argv", ["bounded", str(path), "acp"]), \
                mock.patch.object(sys, "stdin", client()), mock.patch.object(sys, "stdout", io.StringIO()):
            bounded.main()
        rows = bounded.rows(log)
        expected = 1 if scenario == "success" and not control else 0
        self.assertEqual(sum(row["event"] == "model_request_forwarded" for row in rows), expected)
        self.assertEqual(json.loads(ledger.read_text())["cursor"]["sent_or_uncertain"], 2 + expected)
        self.assertTrue(any(row["event"] == "provider_exit" and row["child_exited"] for row in rows))
        self.assertFalse(any(row["event"] == "provider_tool_call" for row in rows))
        for parent, original in self.parents.items():
            self.assertEqual(parent.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
