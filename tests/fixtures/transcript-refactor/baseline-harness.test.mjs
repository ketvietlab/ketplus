import { chmodSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { spawnSync } from 'node:child_process'
import test from 'node:test'
import assert from 'node:assert/strict'
import { buildManifest, defaultSeed } from './generate-transcript-workloads.mjs'

const repoRoot = resolve(new URL('../../..', import.meta.url).pathname)
const harness = join(repoRoot, 'benchmarks/transcript-refactor-baseline.mjs')

function runHarness(args, options = {}) {
  return spawnSync(process.execPath, [harness, ...args], {
    cwd: repoRoot,
    encoding: 'utf8',
    ...options,
  })
}

function executableScript(dir, name, body) {
  const path = join(dir, name)
  writeFileSync(path, body)
  chmodSync(path, 0o755)
  return path
}

function successGenerator(dir) {
  return executableScript(dir, 'generator-ok.mjs', `#!/usr/bin/env node
import { mkdirSync, writeFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
const manifestIndex = process.argv.indexOf('--manifest')
const manifest = manifestIndex >= 0 ? process.argv[manifestIndex + 1] : ''
if (manifest) {
  const target = resolve(manifest)
  mkdirSync(dirname(target), { recursive: true })
  writeFileSync(target, ${JSON.stringify(JSON.stringify(buildManifest()))})
}
`)
}

test('fixture generation failure exits nonzero and skips requested benchmark', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-fail-'))
  const out = join(dir, 'report.json')
  const generator = executableScript(dir, 'generator-fail.mjs', `#!/usr/bin/env node
process.stderr.write('generator failed on purpose\\n')
process.exit(1)
`)
  const result = runHarness(['--generator-script', generator, '--out', out, '--run-daemon-restore'])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.notEqual(result.status, 0)
  assert.equal(report.fixture.generation.status, 'failed')
  assert.equal(report.metrics.daemonRestore.requested, true)
  assert.equal(report.metrics.daemonRestore.status, 'not_run')
})

for (const [name, artifact] of [
  ['missing', null], ['malformed', '{'],
  ['wrong seed', JSON.stringify(buildManifest('wrong'))],
  ['wrong version', JSON.stringify({ ...buildManifest(), version: 999 })],
  ['wrong integrity', JSON.stringify({ ...buildManifest(), scenarios: [] })],
]) {
  test(`exit zero with ${name} artifact fails validation and skips benchmark`, () => {
    const dir = mkdtempSync(join(tmpdir(), 'ketplus-artifact-'))
    const out = join(dir, 'report.json')
    const manifest = join(dir, 'manifest.json')
    // Even a valid stale destination must not rescue a generator that writes nothing.
    writeFileSync(manifest, JSON.stringify(buildManifest(defaultSeed)))
    const generator = executableScript(dir, 'stub.mjs', `import { writeFileSync } from 'node:fs';
      ${artifact === null ? '' : `writeFileSync(process.argv[process.argv.indexOf('--manifest') + 1], ${JSON.stringify(artifact)});`}`)
    const result = runHarness(['--out', out, '--manifest', manifest, '--generator-script', generator, '--run-daemon-restore'])
    assert.notEqual(result.status, 0)
    const report = JSON.parse(readFileSync(out))
    assert.equal(report.fixture.generation.subprocessStatus, 'completed')
    assert.equal(report.fixture.generation.artifactValidation, 'failed')
    assert.equal(report.metrics.daemonRestore.status, 'not_run')
  })
}

test('report/manifest collision fails before subprocess', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-collision-'))
  const out = join(dir, 'same.json')
  const result = runHarness(['--out', out, '--manifest', out, '--run-daemon-restore'])
  assert.notEqual(result.status, 0)
  const report = JSON.parse(readFileSync(out))
  assert.equal(report.fixture.generation.requested, false)
  assert.equal(report.metrics.daemonRestore.status, 'not_run')
})

test('missing generator executable is reported as failed', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-missing-'))
  const out = join(dir, 'report.json')
  const result = runHarness(['--generator-script', join(dir, 'missing.mjs'), '--out', out])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.notEqual(result.status, 0)
  assert.equal(report.fixture.generation.status, 'failed')
  assert.match(report.fixture.generation.stderr, /Cannot find module|MODULE_NOT_FOUND/)
})

test('empty seed failure propagates from generator and skips requested benchmark', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-empty-seed-'))
  const out = join(dir, 'report.json')
  const result = runHarness(['--seed', '', '--out', out, '--run-daemon-restore'])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.notEqual(result.status, 0)
  assert.equal(report.fixture.generation.status, 'failed')
  assert.match(report.fixture.generation.stderr, /--seed must not be empty/)
  assert.equal(report.metrics.daemonRestore.status, 'not_run')
})

test('unsupported release build dir is rejected with diagnostic report', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-args-'))
  const out = join(dir, 'report.json')
  const result = runHarness(['--release-build-dir', 'build/release', '--out', out])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.equal(result.status, 2)
  assert.match(result.stderr, /--release-build-dir is not supported/)
  assert.equal(report.fixture.generation.status, 'failed')
  assert.equal(report.metrics.daemonRestore.status, 'not_run')
})

test('requested daemon restore failure exits nonzero after successful fixture generation', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-restore-'))
  const out = join(dir, 'report.json')
  const manifest = join(dir, 'manifest.json')
  const generator = successGenerator(dir)
  const restore = executableScript(dir, 'restore-fail.sh', `#!/bin/sh
echo restore failed >&2
exit 7
`)
  const result = runHarness(['--generator-script', generator, '--manifest', manifest, '--daemon-restore-script', restore, '--run-daemon-restore', '--out', out])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.equal(report.fixture.generation.status, 'completed')
  assert.equal(report.metrics.daemonRestore.status, 'failed')
  assert.equal(report.metrics.daemonRestore.exitCode, 7)
  assert.notEqual(result.status, 0)
})

test('native capture permission does not mark capture measured', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-harness-capture-'))
  const out = join(dir, 'report.json')
  const manifest = join(dir, 'manifest.json')
  const generator = successGenerator(dir)
  const result = runHarness(['--generator-script', generator, '--manifest', manifest, '--native-capture-granted', '--out', out])
  const report = JSON.parse(readFileSync(out, 'utf8'))

  assert.equal(result.status, 0)
  assert.equal(report.metrics.nativeViewportCapture.requested, true)
  assert.equal(report.metrics.nativeViewportCapture.status, 'not_measured')
})
