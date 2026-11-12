"""Validate local release identity without signing, uploading or network access."""
import argparse
import ipaddress
from pathlib import Path
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET

NAMESPACE = "urn:ketplus:release"
ET.register_namespace("ketplus", NAMESPACE)


def validate_prefix(prefix, channel, architecture, mode):
    url = urlsplit(prefix)
    loopback = False
    try:
        loopback = ipaddress.ip_address(url.hostname or "").is_loopback
    except ValueError:
        pass
    allowed_scheme = url.scheme == "https" or (mode == "local-qa" and url.scheme == "http" and loopback)
    if (not allowed_scheme or not url.hostname or url.username or url.password
            or url.query or url.fragment or not prefix.endswith("/")):
        raise ValueError("download prefix must be HTTPS (or HTTP loopback in local-qa) without credentials, query or fragment")
    parts = unquote(url.path).split("/")
    if any(part in (".", "..") for part in parts):
        raise ValueError("download prefix contains traversal segments")
    if parts[-3:] != [channel, architecture, ""]:
        raise ValueError("download prefix must end with /<channel>/<arch>/")


def process_feed(path, channel, architecture, prefix):
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    tree = ET.parse(path, parser=parser)
    node = tree.getroot().find("channel")
    if node is None:
        raise ValueError("appcast must contain an RSS channel")
    for key, value in {"releaseChannel": channel, "architecture": architecture}.items():
        tag = "{" + NAMESPACE + "}" + key
        entries = node.findall(tag)
        if len(entries) > 1 or (entries and entries[0].text != value):
            raise ValueError("appcast channel/architecture identity mismatch")
        if not entries:
            raise ValueError("existing appcast lacks identity; use a clean scoped directory")
    for enclosure in node.findall("item/enclosure"):
        url = enclosure.get("url", "")
        if not url.startswith(prefix):
            raise ValueError("appcast archive URL does not match release download prefix")
        tail = url[len(prefix):]
        if (not tail or "/" in unquote(tail) or unquote(tail) in (".", "..")
                or "?" in tail or "#" in tail):
            raise ValueError("appcast archive must be directly in the scoped directory")
    # Existing XML is validation-only: preserve prefixes, declarations and signature bytes.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("paths", "check-feed", "seed-feed"))
    parser.add_argument("--mode", choices=("release", "local-qa"), default="release")
    parser.add_argument("--channel", required=True, choices=("staging", "production"))
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--download-prefix", required=True)
    parser.add_argument("--updates-dir")
    parser.add_argument("--compiled-feed")
    parser.add_argument("--appcast")
    args = parser.parse_args()
    try:
        validate_prefix(args.download_prefix, args.channel, args.arch, args.mode)
        if args.operation == "paths":
            if not args.updates_dir or not args.compiled_feed:
                raise ValueError("output directory and compiled feed are required")
            directory = Path(args.updates_dir).resolve()
            if directory.name != args.arch or directory.parent.name != args.channel:
                raise ValueError("updates directory must end with <channel>/<arch> after symlink resolution")
            if args.compiled_feed != args.download_prefix + "appcast.xml":
                raise ValueError("compiled feed does not match release download prefix")
        else:
            if not args.appcast:
                raise ValueError("appcast is required")
            if args.operation == "seed-feed" and not Path(args.appcast).exists():
                root = ET.Element("rss", version="2.0")
                node = ET.SubElement(root, "channel")
                ET.SubElement(node, "title").text = "ketPlus Pro"
                ET.SubElement(node, "{" + NAMESPACE + "}releaseChannel").text = args.channel
                ET.SubElement(node, "{" + NAMESPACE + "}architecture").text = args.arch
                ET.ElementTree(root).write(args.appcast, encoding="utf-8", xml_declaration=True)
            process_feed(args.appcast, args.channel, args.arch, args.download_prefix)
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, "update-release-policy: " + str(error) + "\n")


if __name__ == "__main__":
    main()
