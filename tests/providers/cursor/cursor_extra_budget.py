"""Additional synthetic Cursor allowance; the caller holds the ledger lock."""
import hashlib
import json
import pathlib


PURPOSES = {"canonical_failure_diagnostic", "completed_history_refresh",
            "injected_epoch_inventory", "synthetic_recall"}


def validate(config, budget, provider):
    assert provider == "cursor" and config["mode"] == "cursor-extra-10"
    assert budget["scope"] == "cursor-extra-10"
    assert budget["maximum_total"] == budget["maximum_per_provider"] == 10
    assert budget["allowed_providers"] == ["cursor"]
    assert budget["owner_conversation"] == config["owner"] and config["owner"] > 0
    assert config["head"] and config["purpose"] in PURPOSES
    assert budget["retry_allowed"] is False and type(budget["recall_allowed"]) is bool
    assert config["purpose"] != "synthetic_recall" or budget["recall_allowed"]
    source = budget["authorization"]
    assert source["kind"] == "direct_user_instruction"
    assert source["reference"] == config["authorization_reference"] and source["reference"]
    assert type(source["conversation"]) is int and source["conversation"] > 0
    assert all(type(source[key]) is str and source[key] for key in ["date", "quote", "relay"])
    parents = budget["parent_ledgers"]
    assert [row["decision"] for row in parents] == ["D-3", "D-7"]
    parsed = []
    for parent in parents:
        content = pathlib.Path(parent["path"]).read_bytes()
        assert hashlib.sha256(content).hexdigest() == parent["sha256"]
        data = json.loads(content)
        assert data["decision"] == parent["decision"]
        assert data["owner_conversation"] == config["owner"]
        parsed.append(data)
    assert all(parsed[0][name]["sent_or_uncertain"] == 1 for name in ["codex", "cursor", "copilot"])
    assert parsed[1]["cursor"]["sent_or_uncertain"] == 1
    assert parsed[1]["previous_ledger_sha256"] == parents[0]["sha256"]
    count = budget["cursor"]["sent_or_uncertain"]
    assert type(count) is int and 0 <= count < 10
    assert len(budget["invocations"]) == count
    assert [row["ordinal"] for row in budget["invocations"]] == list(range(1, count + 1))
    assert all(row["provider"] == "cursor" and row["purpose"] in PURPOSES
               for row in budget["invocations"])


def record_reservation(config, budget, model, session_id):
    validate(config, budget, "cursor")
    ordinal = budget["cursor"]["sent_or_uncertain"] + 1
    budget["cursor"].update(sent_or_uncertain=ordinal)
    budget["invocations"].append(dict(ordinal=ordinal, provider="cursor", purpose=config["purpose"],
                                      head=config["head"], model=model, session_id=session_id,
                                      status="reserved_before_delivery"))
