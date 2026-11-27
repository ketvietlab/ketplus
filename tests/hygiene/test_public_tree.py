"""Public-tree boundary guard. Diagnostics print paths and rule names, never contents."""
import ipaddress
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
# Only this guard contains intentional forbidden examples. No documentation exemptions.
ADDRESS_KIND_FILES = {"apps/browser/src/network-address.mjs", "apps/browser/src/origin-policy.mjs"}
ALLOWLIST = {"tests/hygiene/test_public_tree.py"}
RULES = {
    "private repository": re.compile(r"(?<![\w.])(?:ketviet|ketplus-app|ket-turn|ketviet-marketing|ketchat-desktop)(?![\w]|-icon|\.vn\b)", re.I),
    "private module": re.compile(r"\b(?:ketplus_control|ketviewer_control)\b", re.I),
    "private source path": re.compile(r"(?:infra/k8s/environments/production/|apps/ketsuite/|ketsuite/packages/|apps/ios/(?:Sources/KetPlusRemote|KetPlusRemote))", re.I),
    "private infrastructure": re.compile(r"(?:\bkvv2[-_][\w.-]+|[\w.-]+\.svc\.cluster\.local)"),
    "private revision": re.compile(r"(?:private (?:mobile|relay|service|repository)[^\n]{0,80}\b[0-9a-f]{7,40}\b|PR #\d+[^\n]{0,80}\b[0-9a-f]{7,40}\b[^\n]{0,80}backend monorepo)", re.I),
}
IP = re.compile(r"(?<![\w.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![\w.])")

def violations(name, text):
    if name in ALLOWLIST:
        return []
    # Publisher attribution and public asset/API names are not repository references.
    text = re.sub(r"KETVIET JSC", "publisher", text, flags=re.I)
    found = [rule for rule, pattern in RULES.items() if pattern.search(name + "\n" + text)]
    # Synthetic network tests exercise address classification, not deployment hosts.
    fixture = name.startswith(("tests/", "core/tests/", "apps/browser/test/")) or name in ADDRESS_KIND_FILES
    for match in IP.finditer(text):
        try:
            address = ipaddress.IPv4Address(match.group())
        except ipaddress.AddressValueError:
            continue  # Version strings such as 157.0.8103.51 are not addresses.
        if address.is_loopback or address.is_unspecified or fixture:
            continue
        if any(address in ipaddress.IPv4Network(net) for net in ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24")):
            continue
        if address.is_private or address in ipaddress.IPv4Network("100.64.0.0/10"):
            found.append("deployment address")
    return sorted(set(found))

class PublicTreeTest(unittest.TestCase):
    def test_guard_rejects_leaks_without_printing_source(self):
        for payload in (
            "https://github.com/ketvietlab/ketviet/blob/abcdef1/secret.ts",
            "https://github.com/ketvietlab/ketplus-app.git",
            "git@github.com:ketvietlab/ketviet.git", "It lives in ketviet.",
            "kvv2_production", "infra/k8s/environments/production/values.yaml",
            "PR #2177 at e8adee2d in the backend monorepo",
            "ketplus-app at abcdef1", "ket-turn PR #123",
            "private mobile revision abcdef1", "apps/ketsuite/modules/ketplus_control/model.ts",
            "ketplus_control.SessionEvent", "ketviewer_control", "kvv2-production",
            "relay.internal.svc.cluster.local", "host=10.33.44.55",
        ):
            self.assertTrue(violations("docs/example.md", payload))

    def test_private_filename(self):
        self.assertTrue(violations("docs/ketplus-app.md", "neutral text"))

    def test_narrow_public_exceptions(self):
        for payload in ("KETVIET JSC", "https://ketviet.vn", "@ketvietlab/design-system",
                        "ketplus_control_crypto", "ketplus_remote_control", "ketplus-app-icon.png"):
            self.assertEqual(violations("src/public.cpp", payload), [])
        self.assertEqual(violations("apps/browser/test/origin-proxy.test.mjs", "100.97.50.8"), [])
        self.assertTrue(violations("docs/deployment.md", "100.97.50.8"))

    def test_tracked_public_tree(self):
        names = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
        failures = []
        for name in names:
            path = ROOT / name
            if not path.is_file():
                continue
            try:
                data = path.read_bytes()
                if b"\0" in data:
                    continue
                text = data.decode("utf-8")
            except (UnicodeError, OSError):
                continue
            failures.extend(f"{name}: {rule}" for rule in violations(name, text))
        self.assertEqual(failures, [], "Public boundary violations (contents redacted):\n" + "\n".join(failures))

if __name__ == "__main__":
    unittest.main()
