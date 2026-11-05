# Architecture decision records

Use ADRs for durable decisions that affect more than one executable, module, or
repository. Examples include protocol formats, trust boundaries, source/license
ownership, compatibility policy, and deployment topology.

Create `NNNN-short-title.md` from this structure:

```text
# NNNN: Decision title

Status: proposed | accepted | superseded
Date: YYYY-MM-DD

## Context
## Decision
## Consequences
## Alternatives considered
```

Do not use an ADR for routine implementation detail. When superseding a
decision, retain the old record and link both documents.
