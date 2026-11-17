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

    def mock_allowance(self, provider_name):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            provider = root / "fake-provider"
            provider.write_text("#!" + sys.executable + "\nPROVIDER=" + repr(provider_name) + "\nLEDGER=" + repr(str(root / "mock-budget.json")) + "\n" + '''import sys,json
for line in sys.stdin:
 r=json.loads(line)
 if r["method"]=="session/prompt" and r["params"]["prompt"]==[{"type":"text","text":"/model --session auto"}]:
  print(json.dumps({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"config_option_update","configOptions":[]}}}),flush=True)
 elif r["method"]=="session/prompt": assert json.loads(open(LEDGER).read())[PROVIDER]["sent_or_uncertain"]==1
 value={"configOptions":[{"id":"model","currentValue":"claude-haiku-5-5"}]} if r["method"]=="session/set_config_option" else {"stopReason":"end_turn"}
 print(json.dumps({"jsonrpc":"2.0","id":r["id"],"result":value}),flush=True)
''')
            provider.chmod(0o700)
            ledger = root / "mock-budget.json"
            ledger.write_text(json.dumps(dict(decision="D-3", maximum_per_provider=1, owner_conversation=1,
                                             recall_allowed=False, retry_allowed=False,
                                             copilot_auto_decision="D-6", **{provider_name:dict(sent_or_uncertain=0)})))
            log = root / "events.jsonl"
            log.write_text(json.dumps(dict(event="tools_list", epoch="one", pid=0)) + "\n")
            config = root / "launch.json"
            config.write_text(json.dumps(dict(executable=str(provider), work=str(root), log=str(log),
                                             mode="D-3", ledger=str(ledger), head="mock-no-provider", owner=1,
                                             provider=provider_name)))
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
                send(2, "session/prompt", prompt)
                self.assertEqual(receive()["id"], 2)
                self.assertEqual(json.loads(ledger.read_text())[provider_name]["sent_or_uncertain"], 1)
                self.assertEqual(stat.S_IMODE(ledger.stat().st_mode), 0o600)
                send(3, "session/prompt", prompt)
                process.wait(timeout=5)
                events = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(sum(row["event"] == "model_request_forwarded" for row in events), 1)
                self.assertTrue(any(row["event"] == "provider_exit" and row["child_exited"] for row in events))
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                process.stdin.close()
                process.stdout.close()


if __name__ == "__main__":
    unittest.main()
