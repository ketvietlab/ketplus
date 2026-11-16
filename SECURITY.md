# Reporting a security vulnerability

Do not publish suspected vulnerabilities, exploit steps, credentials, private
logs, or affected user data in public issues, pull requests, or attachments.

## Private reporting channel

As of 2026-11-16, GitHub private vulnerability reporting is disabled for this
repository, and no alternative private security contact is documented here.
A repository owner needs to establish a private reporting channel; this guide
does not create an email address, response-time commitment, or disclosure policy.

Check the repository's
[Security page](https://github.com/ketvietlab/ketplus/security) for an updated
private reporting option. If GitHub offers **Report a vulnerability**, use that
private form. If no private option is available, keep the report details private.
You may open a minimal public issue asking maintainers to establish a private
contact, without identifying affected components, impact, reproduction steps,
or attachments. Wait for a verified private channel before sending the report.

## Preparing a private report

Once a private channel is available, include the affected edition and version
or commit, OS/architecture, expected security boundary, impact, and a minimal
synthetic reproduction. Remove credentials and personal data. Test only with
accounts and systems you are authorized to use; do not probe hosted services or
other users' sessions to demonstrate a bug.

For ordinary, non-security bugs, use the public issue templates and
[contribution guide](CONTRIBUTING.md).
