# SPDX-License-Identifier: MPL-2.0
"""Install reviewed, version-and-digest-bound notices omitted by upstream npm archives."""
import json

from artifact_inventory import digest


def install_supplements(prefix, repository, manifest):
    for item in manifest["supplements"]:
        package = prefix / item["packageRoot"]
        if not package.exists():
            continue  # A removed transitive dependency needs no supplement.
        package.resolve(strict=True).relative_to(prefix.resolve(strict=True))
        metadata = package / "package.json"
        data = json.loads(metadata.read_text())
        if (data.get("name") != item["name"] or data.get("version") != item["version"]
                or data.get("license") != item["declaredLicense"] or digest(metadata) != item["manifestSha256"]):
            raise ValueError("Notice supplement package changed; re-review its provenance")
        source = repository / item["noticeFile"]
        source.resolve(strict=True).relative_to(repository.resolve(strict=True))
        if digest(source) != item["noticeSha256"]:
            raise ValueError("Notice supplement differs from its reviewed source")
        target = package / "LICENSE"
        if target.is_symlink():
            raise ValueError("Notice supplement target must not be a symlink")
        if target.exists():
            if digest(target) != item["noticeSha256"]:
                raise ValueError("Existing upstream notice changed; never overwrite it")
        else:
            with target.open("xb") as output:
                output.write(source.read_bytes())
