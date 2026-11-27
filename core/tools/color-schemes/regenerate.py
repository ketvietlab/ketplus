#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Fetch only locked theme inputs, verify SHA-256, then run the production importers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REPO = ROOT.parent


def fetch_sources(lock, cache, offline):
    for repository in lock['repositories']:
        root = cache / repository['key']
        for source in repository['files']:
            path = root / source['path']
            if not path.exists():
                if offline:
                    raise RuntimeError(f'Missing cached source: {path}')
                path.parent.mkdir(parents=True, exist_ok=True)
                # curl uses the host certificate store; never disable TLS verification.
                subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                                '--proto', '=https', '--proto-redir', '=https', '--max-filesize', '26214400',
                                source['url'], '-o', str(path)], check=True)
            if hashlib.sha256(path.read_bytes()).hexdigest() != source['sha256']:
                raise RuntimeError(f'Source hash mismatch: {path}')

        for notice in repository['licenseFiles']:
            expected = (root / notice['source']).read_bytes()
            for directory in [REPO, ROOT]:
                installed = directory / 'third_party/licenses' / notice['distributionFile']
                if installed.read_bytes() != expected:
                    raise RuntimeError(f'License text differs: {installed}')


def prepare_jetbrains(cache):
    root = cache / 'jetbrains'
    resources = root / 'platform/platform-resources/src'
    prepared = root / '_prepared'
    prepared.mkdir(exist_ok=True)
    # Trusted, hash-verified repository XML, not arbitrary user input. The app
    # still reads the resulting standalone XML through its safe Qt XML importer.
    component = ET.parse(resources / 'DefaultColorSchemesManager.xml').getroot()
    darcula = next(s for s in component.findall('scheme') if s.get('name') == 'Darcula')
    ET.ElementTree(darcula).write(prepared / 'darcula.xml', encoding='utf-8')
    for slug, filename, editor in [('darcula', 'darcula.theme.json', 'darcula.xml'),
                                  ('intellij-light', 'Light.theme.json',
                                   '/platform/platform-resources/src/themes/Light.xml')]:
        theme = json.loads((resources / 'themes' / filename).read_text())
        theme['editorScheme'] = editor
        (prepared / (slug + '.theme.json')).write_text(json.dumps(theme, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--cache', type=Path, default=REPO / '.cache/builtin-scheme-sources')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--check', action='store_true', help='Compare without modifying committed data')
    args = parser.parse_args()
    lock_path = HERE / 'sources.lock.json'
    lock = json.loads(lock_path.read_text())
    fetch_sources(lock, args.cache, args.offline)
    prepare_jetbrains(args.cache)
    schemes = ROOT / 'resources/color-schemes'
    reports = REPO / 'docs/theme-import-reports'
    with tempfile.TemporaryDirectory(prefix='ketplus-schemes-') as temporary:
        output = Path(temporary)
        subprocess.run([str(args.generator.resolve()), str(lock_path), str(args.cache.resolve()),
                        str(output / 'schemes'), str(output / 'reports')], check=True)
        for spec in lock['schemes']:
            slug = spec['slug']
            for generated, target in [(output / 'schemes' / (slug + '.ketscheme.json'),
                                       schemes / (slug + '.ketscheme.json')),
                                      (output / 'reports' / (slug + '.json'), reports / (slug + '.json'))]:
                data = generated.read_bytes()
                if args.check:
                    if not target.exists() or target.read_bytes() != data:
                        raise RuntimeError(f'Generated data differs: {target}')
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)


if __name__ == '__main__':
    main()
