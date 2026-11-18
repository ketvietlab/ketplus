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


def inventory_origin(config, budget):
    assert config["mode"] in ["control", "cursor-extra-10"]
    validate(dict(config, mode="cursor-extra-10"), budget, "cursor")
    assert config["purpose"] == "injected_epoch_inventory" and config["model"] == "composer-2.5"
    assert config["attestation"] is True and budget["cursor"]["sent_or_uncertain"] == 2
    origin = budget["invocations"][1]
    assert origin["ordinal"] == 2 and origin["purpose"] == "completed_history_refresh"
    assert origin["model"] == "composer-2.5" and origin["status"] == "completed_with_history_replay"
    assert origin["history_verified"] is True and origin["cleanup_confirmed"] is True
    session_id = origin["session_id"]
    assert type(session_id) is str and session_id
    assert config["inventory_resume"] == budget["cursor"]["session_id"] == session_id
    assert config["work"] == budget["cursor"]["synthetic_cwd"]
    content = pathlib.Path(config["origin_summary"]).read_bytes()
    assert hashlib.sha256(content).hexdigest() == origin["summary_sha256"]
    summary = json.loads(content)
    assert summary["head"] == origin["head"] and summary["ordinal"] == summary["used"] == 2
    assert summary["returncode"] == 0 and summary["forwarded"] == 1
    assert summary["history_marker"] is True and summary["cleanup_confirmed"] is True
    assert summary["canonical"][0] == dict(phase="adapter_turn", reason="completed")
    return session_id


def record_reservation(config, budget, model, session_id):
    validate(config, budget, "cursor")
    if config["purpose"] == "injected_epoch_inventory":
        assert model == "composer-2.5" and session_id == inventory_origin(config, budget)
    ordinal = budget["cursor"]["sent_or_uncertain"] + 1
    budget["cursor"].update(sent_or_uncertain=ordinal)
    budget["invocations"].append(dict(ordinal=ordinal, provider="cursor", purpose=config["purpose"],
                                      head=config["head"], model=model, session_id=session_id,
                                      status="reserved_before_delivery"))
