import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'

const executable = process.env.R03_TEST_EXECUTABLE
const fixtures = process.env.R03_FIXTURES
assert.ok(executable && fixtures, 'Set R03_TEST_EXECUTABLE and R03_FIXTURES; tests must not silently skip')

for (const mode of ['failed', 'partial', 'write_failure']) {
  test(`executable rejects ${mode} baseline`, () => {
    const dir = mkdtempSync(join(tmpdir(), 'r03-metrics-'))
    const output = mode === 'write_failure' ? join(dir, 'missing', 'metrics.json') : join(dir, 'metrics.json')
    const result = spawnSync(executable, ['cloudLifecycle'], {
      encoding: 'utf8',
      env: { ...process.env, R03_FIXTURES: mode === 'failed' ? dir : fixtures, R03_METRICS: output },
    })
    assert.notEqual(result.status, 0)
    assert.equal(result.error, undefined)
    if (mode === 'write_failure') assert.match(result.stderr, /Metrics publish failed/)
    else {
      const report = JSON.parse(readFileSync(output))
      assert.equal(report.status, mode)
      assert.equal(report.baselineValid, false)
      assert.equal(report.rawMeasurements.length, 0)
    }
  })
}

test('build-time provenance distinguishes dirty source from its HEAD', () => {
  const dir = mkdtempSync(join(tmpdir(), 'r03-provenance-'))
  const repo = join(dir, 'source')
  mkdirSync(repo)
  const files = [
    'apps/daemon/src/cloud/CloudEventProjection.cpp', 'apps/daemon/src/cloud/CloudEventProjection.h',
    'apps/daemon/src/storage/LocalJournal.cpp', 'apps/daemon/src/storage/LocalJournal.h',
    'apps/daemon/src/storage/LocalJournalActivityGroups.cpp',
    'apps/daemon/src/storage/LocalJournalLargeEvents.cpp',
    'apps/daemon/src/protocol/DaemonProtocol.cpp', 'apps/daemon/src/protocol/DaemonProtocol.h',
  ]
  for (const file of files) {
    mkdirSync(resolve(repo, file, '..'), { recursive: true })
    writeFileSync(join(repo, file), 'synthetic source\n')
  }
  function run(command, args) {
    const result = spawnSync(command, args, { cwd: repo, encoding: 'utf8' })
    assert.equal(result.status, 0, result.stderr)
    return result.stdout.trim()
  }
  run('git', ['init'])
  run('git', ['add', '.'])
  run('git', ['-c', 'user.name=Synthetic Test', '-c', 'user.email=synthetic@example.invalid', 'commit', '-m', 'synthetic provenance'])
  const head = run('git', ['rev-parse', 'HEAD'])
  const script = new URL('./BuildProvenance.cmake', import.meta.url).pathname
  const output = join(dir, 'provenance.h')
  run('cmake', [`-DREPO=${repo}`, `-DOUTPUT=${output}`, '-P', script])
  const clean = readFileSync(output, 'utf8')
  assert.match(clean, /R03_SOURCE_DIRTY 0/)
  assert.ok(clean.includes(head))
  writeFileSync(join(repo, files[0]), 'changed synthetic source\n')
  run('cmake', [`-DREPO=${repo}`, `-DOUTPUT=${output}`, '-P', script])
  const dirty = readFileSync(output, 'utf8')
  assert.match(dirty, /R03_SOURCE_DIRTY 1/)
  assert.ok(dirty.includes(head))
  assert.notEqual(clean.match(/R03_SOURCE_DIGEST "(.*)"/)[1], dirty.match(/R03_SOURCE_DIGEST "(.*)"/)[1])
})
