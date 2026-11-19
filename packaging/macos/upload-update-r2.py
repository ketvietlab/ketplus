#!/usr/bin/env python3
"""Publish or restore a prepared Sparkle appcast in Cloudflare R2."""
import argparse
import base64
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET


POLICY = runpy.run_path(str(Path(__file__).with_name("update-release-policy.py")))
SPARKLE = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"
VERSION = r"[0-9][A-Za-z0-9.+_-]*"
ARCHIVE = re.compile(r"KetPlus-(" + VERSION + r")\.zip")
SNAPSHOT = re.compile(VERSION + r"/[0-9a-f]{64}\.xml")
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9.+_-]*")
MIME = {".zip": "application/zip", ".delta": "application/octet-stream",
        ".html": "text/html; charset=utf-8", ".md": "text/markdown; charset=utf-8"}
APPCAST = "application/rss+xml"
LIVE_CACHE = "public, max-age=60, must-revalidate"
ASSET_CACHE = "public, max-age=31536000, immutable"
# R2's per-request limit is 5 GiB minus 5 MiB.
MAX_PUT_BYTES = 5 * 1024**3 - 5 * 1024**2


def asset_name(url, prefix, allowed_suffixes):
    if not url.startswith(prefix):
        raise ValueError("appcast asset URL does not match the public prefix")
    name = unquote(url[len(prefix):])
    if not SAFE_NAME.fullmatch(name) or Path(name).suffix not in allowed_suffixes:
        raise ValueError("appcast asset URL must name a supported file directly under the public prefix")
    return name


def read_feed(data, args):
    prefix = args.download_url_prefix
    channel = POLICY["process_feed"](io.BytesIO(data), args.channel, args.arch, prefix, "release")
    items = channel.findall("item")
    assets = {}
    version = None
    for item in items:
        enclosures = item.findall("enclosure")
        if len(enclosures) != 1:
            raise ValueError("each appcast item must have one full archive enclosure")
        archive = asset_name(enclosures[0].get("url", ""), prefix, (".zip",))
        match = ARCHIVE.fullmatch(archive)
        if not match:
            raise ValueError("unexpected full archive filename")
        if version is None:
            version = match[1]
        for enclosure in item.findall(".//enclosure"):
            name = asset_name(enclosure.get("url", ""), prefix, (".zip", ".delta"))
            length = enclosure.get("length", "")
            if not re.fullmatch(r"[0-9]{1,12}", length) or not 0 < int(length) <= MAX_PUT_BYTES:
                raise ValueError("archive or delta length is missing or exceeds the single PUT limit")
            if assets.setdefault(name, int(length)) != int(length):
                raise ValueError("conflicting lengths for the same appcast asset")
        for notes in item.findall(SPARKLE + "releaseNotesLink"):
            name = asset_name(notes.text or "", prefix, (".html", ".md"))
            assets[name] = None
    if version is None:
        raise ValueError("appcast contains no full archive")
    return version, assets


@dataclass
class Payload:
    path: Path
    size: int
    sha256: str
    content_md5: str

    @classmethod
    def read(cls, path):
        if path.is_symlink() or not path.is_file():
            raise ValueError("required release file must be a regular file: " + path.name)
        sha256 = hashlib.sha256()
        md5 = hashlib.md5()
        size = 0
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024**2), b""):
                size += len(chunk)
                if size > MAX_PUT_BYTES:
                    raise ValueError("release file exceeds the single PUT limit: " + path.name)
                sha256.update(chunk)
                md5.update(chunk)
        return cls(path, size, sha256.hexdigest(), base64.b64encode(md5.digest()).decode("ascii"))


def require_size(name, actual, expected):
    if expected is not None and actual != expected:
        raise ValueError("file length does not match the appcast: " + name)


class R2:
    def __init__(self, scope):
        self.scope = scope
        self.env = dict(os.environ, AWS_PAGER="", AWS_CLI_AUTO_PROMPT="off", AWS_EC2_METADATA_DISABLED="true",
                        AWS_IGNORE_CONFIGURED_ENDPOINT_URLS="false", AWS_REQUEST_CHECKSUM_CALCULATION="when_required")
        endpoint = self.env.get("AWS_ENDPOINT_URL_S3", "")
        if not re.fullmatch(r"https://[a-z0-9]+(?:\.(?:eu|fedramp))?\.r2\.cloudflarestorage\.com/?", endpoint):
            raise ValueError("set AWS_ENDPOINT_URL_S3 to the owner's HTTPS R2 S3 endpoint")
        self.bucket = self.env.get("KETPLUS_R2_BUCKET", "")
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]{1,61}[a-z0-9]", self.bucket):
            raise ValueError("set KETPLUS_R2_BUCKET to the owner's R2 bucket name")
        checkout = Path(__file__).resolve().parents[2]
        for variable, filename in (("AWS_CONFIG_FILE", "config"), ("AWS_SHARED_CREDENTIALS_FILE", "credentials")):
            location = self.env.get(variable, str(Path.home() / ".aws" / filename))
            for parent in Path(location).expanduser().resolve().parents:
                if parent.exists() and parent.samefile(checkout):
                    raise ValueError("AWS config and credential files must be outside the checkout")

    def request(self, command, key, *options, missing_ok=False):
        result = subprocess.run(
            ["aws", "s3api", command, "--region", "auto", "--output", "json", "--no-cli-pager",
             "--bucket", self.bucket, "--key", self.scope + "/" + key, *options],
            env=self.env, stdin=subprocess.DEVNULL, capture_output=True)
        if result.returncode:
            # The CLI may include endpoints and credential diagnostics in its output.
            code = re.search(rb"\(([A-Za-z0-9]+)\) when calling the \w+ operation", result.stderr)
            if missing_ok and code and code[1] in (b"404", b"NoSuchKey", b"NotFound"):
                return None
            if code and code[1] in (b"409", b"412", b"PreconditionFailed", b"ConditionalRequestConflict"):
                raise ValueError("conditional R2 request conflicted for " + key + "; review the current release and retry")
            raise ValueError("R2 " + command + " failed for " + key + "; check the dedicated profile and bucket access")
        try:
            return json.loads(result.stdout) if result.stdout.strip() else {}
        except ValueError:
            raise ValueError("AWS CLI returned invalid JSON") from None

    def get(self, key, path, *options, missing_ok=False):
        return self.request("get-object", key, *options, str(path), missing_ok=missing_ok)

    def head(self, key):
        return self.request("head-object", key, missing_ok=True)

    def put(self, key, payload, content_type, cache_control, etag=None):
        condition = ("--if-match", etag) if etag else ("--if-none-match", "*")
        self.request("put-object", key, "--body", str(payload.path), "--content-type", content_type,
                     "--cache-control", cache_control, "--content-md5", payload.content_md5, *condition)
        print("Uploaded", self.scope + "/" + key)

    def needs_upload(self, key, payload, scratch):
        existing = self.head(key)
        if existing is None:
            return True
        if (existing.get("ContentLength") != payload.size
                or self.existing_digest(key, scratch, require_etag(existing)) != payload.sha256):
            raise ValueError("immutable R2 object differs: " + self.scope + "/" + key)
        print("Identical; keeping", self.scope + "/" + key)
        return False

    def existing_digest(self, key, scratch, etag):
        self.get(key, scratch, "--if-match", etag)
        return Payload.read(scratch).sha256


def require_etag(response):
    etag = response.get("ETag", "")
    if not re.fullmatch(r'"[A-Za-z0-9-]+"', etag):
        raise ValueError("R2 response did not contain a usable object ETag")
    return etag


def local_release(args, scratch):
    directory = args.updates_dir.resolve()
    if (directory.parent.name, directory.name) != (args.channel, args.arch):
        raise ValueError("updates directory must end with <channel>/<arch> after symlink resolution")
    feed = directory / "appcast.xml"
    if feed.is_symlink() or not feed.is_file():
        raise ValueError("appcast.xml must be a regular file")
    data = feed.read_bytes()
    _, references = read_feed(data, args)
    names = dict(references)
    # Sparkle may embed notes instead of emitting a releaseNotesLink.
    for name in references:
        if ARCHIVE.fullmatch(name):
            for suffix in (".html", ".md"):
                notes = (directory / name).with_suffix(suffix)
                if notes.exists() or notes.is_symlink():
                    names[notes.name] = None
    assets = {}
    for name, length in names.items():
        payload = Payload.read(directory / name)
        require_size(name, payload.size, length)
        assets[name] = payload
    scratch.write_bytes(data)
    return Payload.read(scratch), assets


def snapshot_id(version, payload):
    return version + "/" + payload.sha256 + ".xml"


def run(args, work):
    scope = args.channel + "/" + args.arch
    feed = work / "desired.xml"
    rollback = args.operation == "rollback"
    assets = {}
    if rollback:
        if not SNAPSHOT.fullmatch(args.snapshot):
            raise ValueError("snapshot must be <version>/<sha256>.xml")
        print("Restore", scope + "/snapshots/" + args.snapshot)
    else:
        desired, assets = local_release(args, feed)
        for name in assets:
            print("Ensure immutable", scope + "/" + name)
    print("Snapshot current", scope + "/appcast.xml", "if it exists and differs")
    print("Write last", scope + "/appcast.xml")
    if not args.execute:
        print("Dry run; R2 was not contacted. Use --execute after release approval.")
        return

    r2 = R2(scope)
    scratch = work / "remote-object"
    live = work / "live.xml"
    current = r2.get("appcast.xml", live, missing_ok=True)
    etag = require_etag(current) if current is not None else None
    if rollback:
        if current is None:
            raise ValueError("rollback requires an existing live appcast")
        r2.get("snapshots/" + args.snapshot, feed)
        desired = Payload.read(feed)
        version, references = read_feed(feed.read_bytes(), args)
        if snapshot_id(version, desired) != args.snapshot:
            raise ValueError("snapshot contents do not match its version and digest")
        for name, length in references.items():
            metadata = r2.head(name)
            if metadata is None:
                raise ValueError("snapshot references a missing R2 object: " + name)
            require_size(name, metadata.get("ContentLength"), length)

    live_payload = Payload.read(live) if current is not None else None
    changed = live_payload is None or desired.sha256 != live_payload.sha256
    snapshot_to_save = None
    if changed and live_payload is not None:
        version, _ = read_feed(live.read_bytes(), args)
        prior = "snapshots/" + snapshot_id(version, live_payload)
        if r2.needs_upload(prior, live_payload, scratch):
            snapshot_to_save = prior

    pending = {name: payload for name, payload in assets.items()
               if r2.needs_upload(name, payload, scratch)}
    for name, payload in pending.items():
        r2.put(name, payload, MIME[Path(name).suffix], ASSET_CACHE)
    if changed:
        if snapshot_to_save:
            r2.put(snapshot_to_save, live_payload, APPCAST, "no-store")
        r2.put("appcast.xml", desired, APPCAST, LIVE_CACHE, etag)
    else:
        print("Live appcast already matches", scope)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("operation", choices=("publish", "rollback"))
    parser.add_argument("--channel", required=True, choices=("staging", "production"))
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--download-url-prefix", required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--updates-dir", type=Path)
    inputs.add_argument("--snapshot")
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    if (args.operation == "publish") != (args.updates_dir is not None):
        parser.error("publish requires --updates-dir; rollback requires --snapshot")
    try:
        POLICY["validate_prefix"](args.download_url_prefix, args.channel, args.arch, "release")
        if not re.fullmatch(r"(/[A-Za-z0-9._~-]+)+/", urlsplit(args.download_url_prefix).path):
            raise ValueError("public prefix must use a plain HTTPS path")
        with tempfile.TemporaryDirectory(prefix="ketplus-update-r2-") as temporary:
            run(args, Path(temporary))
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, "upload-update-r2: " + str(error) + "\n")


if __name__ == "__main__":
    main()
