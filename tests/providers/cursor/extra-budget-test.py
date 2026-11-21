import copy
import hashlib
import json
import pathlib
import tempfile
import unittest

from cursor_extra_budget import validate, record_reservation


class ExtraBudgetTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = pathlib.Path(self.directory.name)
        d3 = dict(decision="D-3", owner_conversation=1,
                  **{p: dict(sent_or_uncertain=1) for p in ["codex", "cursor", "copilot"]})
        p3 = root / "d3.json"
        p3.write_text(json.dumps(d3))
        hash3 = hashlib.sha256(p3.read_bytes()).hexdigest()
        d7 = dict(decision="D-7", owner_conversation=1, cursor=dict(sent_or_uncertain=1),
                  previous_ledger_sha256=hash3)
        p7 = root / "d7.json"
        p7.write_text(json.dumps(d7))
        self.originals = {p: p.read_bytes() for p in [p3, p7]}
        self.config = dict(mode="cursor-extra-10", owner=1, head="fixture-head",
                           purpose="canonical_failure_diagnostic", authorization_reference="fixture criterion")
        self.budget = dict(scope="cursor-extra-10", maximum_total=10, maximum_per_provider=10,
                           allowed_providers=["cursor"], owner_conversation=1, retry_allowed=False,
                           recall_allowed=True, cursor=dict(sent_or_uncertain=0), invocations=[],
                           authorization=dict(kind="direct_user_instruction", reference="fixture criterion",
                               conversation=1, date="fixture-date", quote="synthetic authorization", relay="fixture-relay"),
                           parent_ledgers=[dict(decision=d, path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                                           for d, p in [("D-3", p3), ("D-7", p7)]])

    def test_ten_reserved_prompts_preserve_parent_ledgers_and_eleventh_is_denied(self):
        for ordinal in range(1, 11):
            record_reservation(self.config, self.budget, "synthetic-cheap", "synthetic-session")
            self.assertEqual(self.budget["cursor"]["sent_or_uncertain"], ordinal)
        with self.assertRaises(AssertionError):
            record_reservation(self.config, self.budget, "synthetic-cheap", "synthetic-session")
        for path, original in self.originals.items():
            self.assertEqual(path.read_bytes(), original)

    def test_wrong_provider_scope_owner_or_authorization_is_denied(self):
        for provider in ["codex", "copilot"]:
            with self.subTest(provider=provider), self.assertRaises(AssertionError):
                validate(self.config, self.budget, provider)
        for fields in [dict(maximum_total=11), dict(maximum_per_provider=11),
                       dict(allowed_providers=["cursor", "codex"]), dict(owner_conversation=2),
                       dict(retry_allowed=True), dict(scope="D-7")]:
            with self.subTest(fields=list(fields)), self.assertRaises(AssertionError):
                validate(self.config, dict(self.budget, **fields), "cursor")
        for field, value in [("kind", "agent_relay"), ("reference", "wrong"), ("relay", "")]:
            budget = copy.deepcopy(self.budget)
            budget["authorization"][field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError):
                validate(self.config, budget, "cursor")

    def test_count_or_journal_tampering_and_unscoped_purpose_are_denied(self):
        for count in [-1, True, 1, 10, 11]:
            budget = copy.deepcopy(self.budget)
            budget["cursor"]["sent_or_uncertain"] = count
            with self.subTest(count=count), self.assertRaises(AssertionError):
                validate(self.config, budget, "cursor")
        with self.assertRaises(AssertionError):
            validate(dict(self.config, purpose="blind_retry"), self.budget, "cursor")
        with self.assertRaises(AssertionError):
            validate(dict(self.config, purpose="synthetic_recall"),
                     dict(self.budget, recall_allowed=False), "cursor")

    def test_parent_changes_and_false_parent_chain_are_denied(self):
        for path, original in self.originals.items():
            path.write_bytes(original + b" ")
            with self.assertRaises(AssertionError):
                validate(self.config, self.budget, "cursor")
            path.write_bytes(original)
        budget = copy.deepcopy(self.budget)
        parent = pathlib.Path(budget["parent_ledgers"][1]["path"])
        data = json.loads(parent.read_text())
        data["previous_ledger_sha256"] = "other-parent"
        parent.write_text(json.dumps(data))
        budget["parent_ledgers"][1]["sha256"] = hashlib.sha256(parent.read_bytes()).hexdigest()
        with self.assertRaises(AssertionError):
            validate(self.config, budget, "cursor")


if __name__ == "__main__":
    unittest.main()
