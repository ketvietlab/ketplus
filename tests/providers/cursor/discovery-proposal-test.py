#!/usr/bin/env python3
"""Quota-free proposal and allowance tests; only Python fixture processes run."""
import json
import os
import importlib.util
import hashlib
import pathlib
import select
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
PROPOSAL = HERE / "discovery-proposal.py"


class DiscoveryProposalTest(unittest.TestCase):
    def test_explicit_cheap_models_refuse_auto_fast_default_or_mismatched_ack(self):
        spec = importlib.util.spec_from_file_location("bounded", HERE / "adapter-bounded-fixture.py")
        bounded = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bounded)
        config = dict(mode="cursor-extra-10", purpose="completed_history_refresh", model="composer-2.5")
        self.assertEqual(bounded.cursor_model(config), "composer-2.5")
        for field, value in [("model", "auto"), ("model", "default"), ("model", "composer-2.5-fast"),
                             ("mode", "D-7"), ("purpose", "blind_retry")]:
            with self.subTest(field=field, value=value), self.assertRaises(AssertionError):
                bounded.cursor_model(dict(config, **{field: value}))
        for current in ["composer-2.5", "composer-2.5[fast=false]"]:
            options = [dict(id="model", currentValue=current)]
            self.assertTrue(bounded.cheap_model_ack("composer-2.5", options)[0])
            for fast in [False, "false"]:
                self.assertTrue(bounded.cheap_model_ack("composer-2.5", options + [dict(id="fast", currentValue=fast)])[0])
            for fast in [True, "true", 0, None, "unknown"]:
                self.assertEqual(bounded.cheap_model_ack("composer-2.5", options + [dict(id="fast", currentValue=fast)]),
                                 (False, "unmatched"))
        for current in ["auto", "default", "composer-2.5-fast", "composer-2.5[fast=true]", "private-token"]:
            self.assertEqual(bounded.cheap_model_ack("composer-2.5", [dict(id="model", currentValue=current)]),
                             (False, "unmatched"))
        self.assertEqual(bounded.cheap_model_ack("auto", [dict(id="model", currentValue="composer-2.5")]),
                         (False, "unmatched"))

    def test_live_observed_mcp_child_blocks_replacement_before_any_provider_start(self):
        for field in ["pid", "child_pid"]:
            with self.subTest(worker_pid_field=field):
                self.live_worker_blocks_replacement(field)

    def live_worker_blocks_replacement(self, field):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            log = root / "events.jsonl"
            # New observer rows carry the actual worker in child_pid. The log
            # writer PID must not be mistaken for that still-live worker.
            row = dict(event="mcp", pid=0)
            row[field] = os.getpid()
            log.write_text(json.dumps(row) + "\n")
            config = root / "launch.json"
            config.write_text(json.dumps(dict(executable="must-not-start", work=str(root),
                                             log=str(log), mode="control")))
            result = subprocess.run([sys.executable, "-B", str(HERE / "adapter-bounded-fixture.py"),
                                     str(config), "acp"], capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            events = list(map(json.loads, log.read_text().splitlines()))
            self.assertTrue(any(row["event"] == "closure_barrier_failed" and row["prior_mcp_alive"] == 1
                                for row in events))
            self.assertFalse(any(row["event"] == "provider" for row in events))

    def test_real_node_preload_attests_owned_stdio_without_changing_fixture(self):
        for block_tool in [False, True]:
            with self.subTest(block_tool=block_tool):
                self.real_node_preload(block_tool)

    def real_node_preload(self, block_tool):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node needed for the observer fixture only")
        spec = importlib.util.spec_from_file_location("attestation", HERE / "private_attestation.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve() / "proposal"
            subprocess.run([sys.executable, "-B", str(PROPOSAL), "--prepare", "--root", str(root)],
                           capture_output=True, check=True)
            session = json.loads((root / "session-new.json").read_text())
            server = session["mcpServers"][0]
            script = root / "index.js"
            script.write_text('''const {spawn}=require('node:child_process');
const server=JSON.parse(process.argv[2]);
const child=spawn(server.command,server.args,{cwd:process.cwd(),stdio:['pipe','pipe','inherit']});
const send=message=>child.stdin.write(JSON.stringify({jsonrpc:'2.0',...message})+'\\n');
let pending='';
child.stdout.on('data',chunk=>{pending+=chunk;let i;while((i=pending.indexOf('\\n'))>=0){
 const message=JSON.parse(pending.slice(0,i));pending=pending.slice(i+1);
 if(message.id===1){send({method:'notifications/initialized'});send({id:2,method:'tools/list'});}
 if(message.id===2){if(process.argv[3]==='deny')send({id:3,method:'tools/call',params:{name:'t3_probe_status',arguments:{}}});else child.stdin.end();}
}});
send({id:1,method:'initialize',params:{protocolVersion:'2024-11-05'}});
''')
            site = dict(file=str(script), line=3, column=13,
                        sha256=hashlib.sha256(script.read_bytes()).hexdigest())
            observer = module.Attestation(server, session["cwd"], "host-private-epoch", site)
            process = subprocess.Popen([node, str(script), json.dumps(server), "deny" if block_tool else "read"],
                                       cwd=session["cwd"], env=observer.environment(),
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            observer.native_pid = process.pid
            try:
                process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0)
                # Drain asynchronous private socket events without launching another process.
                import time
                end = time.monotonic() + 1
                goal = "forbidden" if block_tool else "inventory"
                while time.monotonic() < end:
                    state = observer.snapshot()
                    if state[goal] and (block_tool or state["revoked"]):
                        break
                    time.sleep(.01)
                self.assertTrue(observer.snapshot()[goal])
                if not block_tool:
                    self.assertTrue(observer.snapshot()["attested"])
                    self.assertTrue(observer.snapshot()["revoked"])
                    self.assertFalse(observer.snapshot()["ready"])
                methods = [json.loads(line)["method"] for line in (root / "events.jsonl").read_text().splitlines()]
                self.assertNotIn("tools/call", methods)
                self.assertEqual(observer.snapshot()["rejected"], 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=3)
                observer.close()

    def test_private_attestation_rejects_spoofs_and_project_only_handshakes(self):
        spec = importlib.util.spec_from_file_location("attestation", HERE / "private_attestation.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        server = dict(command=sys.executable, args=["synthetic"], env=[])
        observer = module.Attestation(server, str(HERE), "private-host-epoch")
        observer.native_pid = 123
        def message(event, **overrides):
            return dict(dict(nonce=observer.nonce, transportEpoch=observer.epoch,
                             launchDigest=observer.digest, observerPid=123, childPid=456,
                             sequence=["spawn", "initialize_sent", "initialize_result",
                                       "initialized_sent", "inventory"].index(event),
                             mainThread=True, injected=True, role="index.js", event=event), **overrides)
        try:
            # A project pool process or worker thread cannot attest injection.
            for event in ["spawn", "initialize_sent", "initialize_result", "initialized_sent"]:
                observer.accept(message(event, observerPid=777, role="connect-pool-worker.js"), 777)
                observer.accept(message(event, mainThread=False), 123)
                observer.accept(message(event, injected=False, childPid=789), 123)
            self.assertFalse(observer.snapshot()["attested"])
            for event in ["spawn", "initialize_sent", "initialize_result"]:
                observer.accept(message(event), 123)
            self.assertFalse(observer.snapshot()["attested"])
            observer.accept(message("initialized_sent"), 123)
            self.assertTrue(observer.snapshot()["attested"])
            self.assertFalse(observer.snapshot()["inventory"])
            observer.accept(message("inventory", valid=True), 123)
            self.assertTrue(observer.snapshot()["inventory"])
            # Wrong nonce, epoch, tuple or OS peer PID poisons readiness.
            for override, peer in [({"nonce": "old"}, 123),
                                   ({"transportEpoch": "old"}, 123),
                                   ({"launchDigest": "other"}, 123), ({}, 999)]:
                observer.accept(message("spawn", **override), peer)
            self.assertEqual(observer.snapshot()["rejected"], 4)
            self.assertFalse(observer.snapshot()["attested"])
            self.assertNotIn(observer.nonce, json.dumps(observer.events))
        finally:
            observer.close()

    def test_project_discovery_cannot_attest_another_injected_instance(self):
        spec = importlib.util.spec_from_file_location("bounded", HERE / "adapter-bounded-fixture.py")
        bounded = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bounded)
        observed = [dict(method="spawn", pid=1), dict(method="tools/list", pid=1),
                    dict(method="spawn", pid=2), dict(method="initialize", pid=2)]
        self.assertFalse(bounded.approved_discovery(observed)[2])
        observed.append(dict(method="tools/list", pid=2))
        self.assertTrue(bounded.approved_discovery(observed)[2])
        self.assertFalse(bounded.approved_discovery([])[2])

    def test_static_inventory_and_matching_launch_tuple(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve() / "proposal"
            subprocess.run([sys.executable, "-B", str(PROPOSAL), "--prepare", "--root", str(root)],
                           capture_output=True, check=True)
            manifest = json.loads((root / "manifest.json").read_text())
            project = json.loads((root / "work/.cursor/mcp.json").read_text())["mcpServers"][manifest["identity"]]
            acp = json.loads((root / "session-new.json").read_text())["mcpServers"][0]
            self.assertEqual((project["command"], project["args"]), (acp["command"], acp["args"]))
            self.assertFalse(manifest["approval_executed"])
            self.assertEqual(stat.S_IMODE((root / "manifest.json").stat().st_mode), 0o600)
            requests = [dict(id=1, method="initialize", params=dict(protocolVersion="2024-11-05")),
                        dict(id=2, method="tools/list"),
                        dict(id=3, method="tools/call", params=dict(name="t3_probe_status", arguments={})),
                        dict(id=4, method="tools/call", params=dict(name="background_task_start", arguments={}))]
            result = subprocess.run([sys.executable, "-B", str(PROPOSAL), "--serve", "--root", str(root)],
                                    input="".join(json.dumps(dict(jsonrpc="2.0", **r)) + "\n" for r in requests),
                                    capture_output=True, text=True, check=True, timeout=5)
            replies = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual([tool["name"] for tool in replies[1]["result"]["tools"]], ["t3_probe_status"])
            self.assertEqual(replies[2]["result"]["content"][0]["text"], "synthetic-ready")
            self.assertIn("error", replies[3])

    def test_mock_allowance_is_written_before_forward_and_cannot_be_reused(self):
        for provider in ["cursor", "copilot"]:
            with self.subTest(provider=provider):
                self.mock_allowance(provider)

    def test_extra_cursor_decision_preserves_consumed_parent_and_cannot_be_reused(self):
        self.mock_allowance("cursor", "D-7")

    def test_ten_extra_prompts_reserve_before_forward_and_eleventh_is_denied(self):
        self.mock_allowance("cursor", "cursor-extra-10")

    def test_composer_non_fast_uses_the_same_reservation_barrier(self):
        self.mock_allowance("cursor", "cursor-extra-10", "composer-2.5")

    def test_extra_cursor_decision_rejects_wrong_scope_or_changed_parent(self):
        spec = importlib.util.spec_from_file_location("bounded", HERE / "adapter-bounded-fixture.py")
        bounded = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bounded)
        with tempfile.TemporaryDirectory() as directory:
            parent = pathlib.Path(directory) / "consumed-d3.json"
            consumed = dict(decision="D-3", owner_conversation=1,
                            **{name: dict(sent_or_uncertain=1) for name in ["codex", "cursor", "copilot"]})
            parent.write_text(json.dumps(consumed))
            budget = dict(decision="D-7", maximum_per_provider=1, maximum_total=1, allowed_providers=["cursor"],
                          owner_conversation=1, recall_allowed=False, retry_allowed=False,
                          previous_ledger=str(parent), previous_ledger_sha256=hashlib.sha256(parent.read_bytes()).hexdigest(),
                          **{name: dict(sent_or_uncertain=0) for name in ["codex", "cursor", "copilot"]})
            config = dict(mode="D-7", owner=1, head="mock-no-provider")
            bounded.validate_allowance(config, budget, "cursor")
            for provider in ["codex", "copilot"]:
                with self.assertRaises(AssertionError):
                    bounded.validate_allowance(config, budget, provider)
            for fields in [dict(maximum_total=2), dict(allowed_providers=["cursor", "codex"]),
                           dict(decision="D-3"), dict(owner_conversation=2), dict(retry_allowed=True),
                           dict(recall_allowed=True), dict(cursor=dict(sent_or_uncertain=1))]:
                with self.subTest(fields=list(fields)), self.assertRaises(AssertionError):
                    bounded.validate_allowance(config, dict(budget, **fields), "cursor")
            consumed["cursor"]["sent_or_uncertain"] = 0
            parent.write_text(json.dumps(consumed))
            with self.assertRaises(AssertionError):
                bounded.validate_allowance(config, budget, "cursor")
            budget["previous_ledger_sha256"] = hashlib.sha256(parent.read_bytes()).hexdigest()
            with self.assertRaises(AssertionError):
                bounded.validate_allowance(config, budget, "cursor")

    def mock_allowance(self, provider_name, decision="D-3", model="claude-haiku-5-5-low"):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            provider = root / "fake-provider"
            provider.write_text("#!" + sys.executable + "\nPROVIDER=" + repr(provider_name) + "\nLEDGER=" + repr(str(root / "mock-budget.json")) + "\nCURRENT=" + repr("composer-2.5[fast=false]" if model == "composer-2.5" else "claude-haiku-5-5") + "\n" + '''import sys,json
forwarded=0
for line in sys.stdin:
 r=json.loads(line)
 if r["method"]=="session/prompt" and r["params"]["prompt"]==[{"type":"text","text":"/model --session auto"}]:
  print(json.dumps({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"config_option_update","configOptions":[]}}}),flush=True)
 elif r["method"]=="session/prompt":
  forwarded+=1
  assert json.loads(open(LEDGER).read())[PROVIDER]["sent_or_uncertain"]==forwarded
 value={"configOptions":[{"id":"model","currentValue":CURRENT}]} if r["method"]=="session/set_config_option" else {"stopReason":"end_turn"}
 print(json.dumps({"jsonrpc":"2.0","id":r["id"],"result":value}),flush=True)
''')
            provider.chmod(0o700)
            ledger = root / "mock-budget.json"
            budget = dict(decision=decision, maximum_per_provider=1, owner_conversation=1,
                          recall_allowed=False, retry_allowed=False,
                          copilot_auto_decision="D-6", **{provider_name: dict(sent_or_uncertain=0)})
            parents = {}
            if decision in ["D-7", "cursor-extra-10"]:
                parent = root / "consumed-d3.json"
                parent_bytes = json.dumps(dict(decision="D-3", owner_conversation=1,
                    **{name: dict(sent_or_uncertain=1) for name in ["codex", "cursor", "copilot"]})).encode()
                parent.write_bytes(parent_bytes)
                parents[parent] = parent_bytes
                budget.update(allowed_providers=["cursor"], maximum_total=1, previous_ledger=str(parent),
                              previous_ledger_sha256=hashlib.sha256(parent_bytes).hexdigest())
            if decision == "cursor-extra-10":
                d7 = root / "consumed-d7.json"
                d7.write_text(json.dumps(dict(decision="D-7", owner_conversation=1, cursor=dict(sent_or_uncertain=1),
                                             previous_ledger_sha256=hashlib.sha256(parent_bytes).hexdigest())))
                parents[d7] = d7.read_bytes()
                budget.pop("decision")  # Direct user instruction, not a new decision answer.
                budget.update(scope="cursor-extra-10", maximum_total=10, maximum_per_provider=10,
                              allowed_providers=["cursor"], recall_allowed=True, invocations=[],
                              authorization=dict(kind="direct_user_instruction", reference="mock criterion",
                                  conversation=1, date="mock-date", quote="synthetic grant", relay="mock-relay"),
                              parent_ledgers=[dict(decision=name, path=str(path), sha256=hashlib.sha256(content).hexdigest())
                                              for name, (path, content) in zip(["D-3", "D-7"], parents.items())])
            ledger.write_text(json.dumps(budget))
            log = root / "events.jsonl"
            log.write_text(json.dumps(dict(event="tools_list", epoch="one", pid=0)) + "\n")
            config = root / "launch.json"
            config.write_text(json.dumps(dict(executable=str(provider), work=str(root), log=str(log),
                                             mode=decision, ledger=str(ledger), head="mock-no-provider", owner=1,
                                             provider=provider_name, model=model,
                                             purpose="completed_history_refresh" if model == "composer-2.5" else "canonical_failure_diagnostic",
                                             authorization_reference="mock criterion")))
            process = subprocess.Popen([sys.executable, "-B", str(HERE / "adapter-bounded-fixture.py"),
                                        str(config), "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, text=True)
            try:
                def send(serial, method, params):
                    process.stdin.write(json.dumps(dict(jsonrpc="2.0", id=serial, method=method, params=params)) + "\n")
                    process.stdin.flush()
                def receive():
                    self.assertTrue(select.select([process.stdout], [], [], 5)[0], "Mock response timed out")
                    return json.loads(process.stdout.readline())
                send(1, "session/set_config_option", dict(configId="model"))
                self.assertEqual(receive()["id"], 1)
                marker = "T3_BOUNDED_COPILOT_ALPHA" if provider_name == "copilot" else "T3_BOUNDED_CURSOR_ALPHA"
                prompt = dict(sessionId="mock-session", prompt=[dict(type="text", text=marker)])
                maximum = 10 if decision == "cursor-extra-10" else 1
                for ordinal in range(1, maximum + 1):
                    send(ordinal + 1, "session/prompt", prompt)
                    self.assertEqual(receive()["id"], ordinal + 1)
                    self.assertEqual(json.loads(ledger.read_text())[provider_name]["sent_or_uncertain"], ordinal)
                self.assertEqual(stat.S_IMODE(ledger.stat().st_mode), 0o600)
                send(maximum + 2, "session/prompt", prompt)
                process.wait(timeout=5)
                events = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(sum(row["event"] == "model_request_forwarded" for row in events), maximum)
                self.assertTrue(any(row["event"] == "provider_exit" and row["child_exited"] for row in events))
                for path, original in parents.items():
                    self.assertEqual(path.read_bytes(), original)
                if decision == "cursor-extra-10":
                    journal = json.loads(ledger.read_text())["invocations"]
                    self.assertEqual([row["ordinal"] for row in journal], list(range(1, 11)))
                    self.assertTrue(all(row["status"] == "reserved_before_delivery" for row in journal))
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                process.stdin.close()
                process.stdout.close()


if __name__ == "__main__":
    unittest.main()
