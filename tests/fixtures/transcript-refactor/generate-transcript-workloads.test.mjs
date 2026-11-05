import { createHash } from 'node:crypto'
import { mkdtempSync, readFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  aggregatedOutputMarker,
  buildManifest,
  commandOutputMarker,
  defaultSeed,
  materialize,
  scenarios,
} from './generate-transcript-workloads.mjs'

function sha256File(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex')
}

test('replay schedule has stable order and per-session cursors with resolvable references', () => {
  for (const scenario of scenarios()) {
    const cursors = new Map()
    if (scenario.category === 'valid') for (const event of scenario.eventStream) {
      const key = `${event.session_id}:${event.session_generation}`
      assert.equal(event.cursor, (cursors.get(key) ?? 0) + 1)
      cursors.set(key, event.cursor)
    }
    for (let index = 1; index < scenario.operations.length; index++) {
      const a = scenario.operations[index - 1], b = scenario.operations[index]
      assert.ok(a.offset_ms < b.offset_ms || (a.offset_ms === b.offset_ms && a.operation_ordinal < b.operation_ordinal))
    }
    for (const op of scenario.operations.filter((value) => value.kind === 'liveEvent')) {
      assert.ok(scenario.eventStream.some((event) => event.session_id === op.session_id && event.session_generation === op.session_generation && event.cursor === op.cursor))
    }
  }
})

function jsonlRecords(path) {
  const text = readFileSync(path, 'utf8')
  if (!text) return []
  return text.trimEnd().split('\n').map((line) => JSON.parse(line))
}

test('manifest hashes are deterministic and content-sensitive', () => {
  const first = buildManifest(defaultSeed)
  const second = buildManifest(defaultSeed)
  const changed = buildManifest(`${defaultSeed}:changed`)

  assert.deepEqual(first, second)
  assert.notEqual(first.scenarios[0].exactJsonl.sha256, changed.scenarios[0].exactJsonl.sha256)
})

test('materialized scenarios match manifest counts bytes and hashes', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-transcript-fixtures-'))
  const manifest = await materialize(defaultSeed, 'all', dir)

  for (const scenario of manifest.scenarios) {
    const eventPath = join(dir, scenario.files.events)
    const operationPath = join(dir, scenario.files.operations)
    const expectedPath = join(dir, scenario.files.expected)
    const eventBytes = readFileSync(eventPath)
    const operationBytes = readFileSync(operationPath)
    const events = jsonlRecords(eventPath)
    const operations = jsonlRecords(operationPath)

    assert.equal(events.length, scenario.eventCount, scenario.name)
    assert.equal(operations.length, scenario.operationCount, scenario.name)
    assert.equal(eventBytes.byteLength, scenario.exactJsonl.bytes, scenario.name)
    assert.equal(operationBytes.byteLength, scenario.operationsJsonl.bytes, scenario.name)
    assert.equal(sha256File(eventPath), scenario.exactJsonl.sha256, scenario.name)
    assert.equal(sha256File(operationPath), scenario.operationsJsonl.sha256, scenario.name)
    assert.doesNotMatch(readFileSync(expectedPath, 'utf8'), /summaryOnly/, scenario.name)
  }
})

test('giant turn materializes a real large body', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'ketplus-transcript-giant-'))
  await materialize(defaultSeed, 'giant-turn', dir)
  const records = jsonlRecords(join(dir, 'giant-turn.events.jsonl'))
  const finalItem = records.at(-1).payload.item

  assert.ok(records.length > 500)
  assert.ok(Buffer.byteLength(finalItem.text) >= 8 * 1024 * 1024)
})

test('command output marker fixture keeps raw output but expected projection strips it', async () => {
  const scenario = scenarios(defaultSeed).find((value) => value.name === 'command-output-cloud-strip-marker')
  assert.ok(JSON.stringify(scenario.eventStream).includes(commandOutputMarker))
  assert.ok(JSON.stringify(scenario.eventStream).includes(aggregatedOutputMarker))

  const expected = JSON.stringify(scenario.expected.cloudProjectionEvents)
  assert.ok(!expected.includes(commandOutputMarker))
  assert.ok(!expected.includes(aggregatedOutputMarker))
  assert.ok(expected.includes(scenario.expected.requiredCommandText))
  assert.ok(expected.includes('"status":"completed"'))
  assert.ok(scenario.expected.requiredCommandText.length > 100)
})

test('negative scenarios are separated from valid replay workloads', () => {
  const manifest = buildManifest(defaultSeed)
  const negatives = manifest.scenarios.filter((scenario) => scenario.category === 'negative')

  assert.deepEqual(
    negatives.map((scenario) => scenario.name),
    ['negative-cursor-gap', 'negative-conflicting-duplicate', 'negative-stale-generation-response'],
  )
  for (const scenario of negatives) {
    assert.ok(scenario.expectations.expectedFailure)
    assert.equal(scenario.expectations.replayable, false)
  }
})
