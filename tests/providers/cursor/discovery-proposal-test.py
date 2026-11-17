#!/usr/bin/env python3
"""Quota-free proposal and allowance tests; only Python fixture processes run."""
import json
import pathlib
import select
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
PROPOSAL = HERE / "discovery-proposal.py"


class DiscoveryProposalTest(unittest.TestCase):
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
