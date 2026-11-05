#!/usr/bin/env node

import { createHash } from 'node:crypto'
import { createWriteStream, mkdirSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import process from 'node:process'
import { once } from 'node:events'

export const defaultSeed = 'ketplus-transcript-refactor-wave1'
export const commandOutputMarker = 'KETPLUS_COMMAND_OUTPUT_SHOULD_NOT_REACH_CLOUD'
export const aggregatedOutputMarker = `${commandOutputMarker}_AGGREGATED`
export const fixtureVersion = 3

const giantSourceBytes = 8 * 1024 * 1024 + 17_408

function usage() {
  return `Usage:
  node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs [options]

Options:
  --seed <value>          Deterministic seed. Default: ${defaultSeed}
  --scenario <name>       Scenario name, or "all". Default: all
  --list                  List scenario names.
  --manifest <path>       Write compact manifest JSON.
  --materialize <dir>     Write replayable JSONL events/operations.
  --self-test             Verify deterministic hashes and command output stripping.
  --help                  Show this help.

The default output is a compact manifest. Materialized JSONL is opt-in so large
fixtures, including the >8 MiB giant turn, are never committed by accident.
`
}

function parseArgs(argv) {
  const args = { seed: defaultSeed, scenario: 'all', manifest: '', materialize: '', selfTest: false, list: false }
  for (let index = 2; index < argv.length; ++index) {
    const arg = argv[index]
    if (arg === '--help' || arg === '-h') args.help = true
    else if (arg === '--self-test') args.selfTest = true
    else if (arg === '--list') args.list = true
    else if (arg === '--seed') args.seed = argv[++index] ?? ''
    else if (arg === '--scenario') args.scenario = argv[++index] ?? ''
    else if (arg === '--manifest') args.manifest = argv[++index] ?? ''
    else if (arg === '--materialize') args.materialize = argv[++index] ?? ''
    else throw new Error(`Unknown argument: ${arg}`)
  }
  if (!args.seed) throw new Error('--seed must not be empty')
  if (!args.scenario) throw new Error('--scenario must not be empty')
  return args
}

function sha256Bytes(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}

function stableJson(value) {
  if (value === null || typeof value !== 'object') return JSON.stringify(value)
  if (Array.isArray(value)) return `[${value.map(stableJson).join(',')}]`
  return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${stableJson(value[key])}`).join(',')}}`
}

function canonicalLine(value) {
  return `${stableJson(value)}\n`
}

function jsonlLine(value) {
  return `${JSON.stringify(value)}\n`
}

function hashAccumulator() {
  const hash = createHash('sha256')
  let bytes = 0
  let count = 0
  return {
    add(line) {
      const buffer = Buffer.from(line)
      hash.update(buffer)
      bytes += buffer.byteLength
      count += 1
    },
    digest(representation) {
      return { representation, count, bytes, sha256: hash.digest('hex') }
    },
  }
}

function rngFrom(seed) {
  let state = Number.parseInt(sha256Bytes(seed).slice(0, 8), 16) >>> 0
  return () => {
    state = (state + 0x6d2b79f5) >>> 0
    let t = state
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

function textOfSize(label, bytes) {
  const prefix = `${label}\n`
  const prefixBytes = Buffer.byteLength(prefix)
  if (prefixBytes >= bytes) return prefix.slice(0, bytes)
  const chunk = '0123456789abcdef Markdown table Mermaid media code block synthetic payload.\n'
  const chunkBytes = Buffer.byteLength(chunk)
  const repeatCount = Math.floor((bytes - prefixBytes) / chunkBytes)
  const tailBytes = bytes - prefixBytes - repeatCount * chunkBytes
  return `${prefix}${chunk.repeat(repeatCount)}${'x'.repeat(tailBytes)}`
}

function event(session, cursor, turn, eventType, payload, fields = {}) {
  return {
    session_id: session,
    session_generation: fields.session_generation ?? 1,
    cursor,
    turn_sequence: turn,
    event_type: eventType,
    payload,
    sent_at: new Date(Date.UTC(2026, 8, 12, 0, 0, cursor % 60, Math.floor(cursor / 60))).toISOString(),
    ...fields.extra,
  }
}

function providerItem(type, id, fields = {}) {
  return { id, type, ...fields }
}

function operation(kind, offsetMs, fields = {}) {
  return { kind, offset_ms: offsetMs, ...fields }
}

function projectForCloud(eventValue) {
  if (eventValue.event_type === 'command.output_delta' || eventValue.event_type === 'command.output_aggregated') return null
  if (eventValue.event_type !== 'item.completed' && eventValue.event_type !== 'item.started') return eventValue
  const item = eventValue.payload.item
  if (item?.type !== 'command') return eventValue
  return {
    ...eventValue,
    payload: {
      ...eventValue.payload,
      item: {
        id: item.id,
        type: item.type,
        command: item.command,
        status: item.status,
        exitCode: item.exitCode ?? null,
      },
    },
  }
}

function commandScenarioExpected(events) {
  return events.map(projectForCloud).filter(Boolean)
}

function buildItemsScenario(seed, itemCount) {
  const session = `selected-${itemCount}`
  const events = []
  let cursor = 1
  for (let index = 0; index < itemCount / 2; ++index) {
    const turn = index + 1
    events.push(event(session, cursor++, turn, 'item.completed', {
      item: providerItem('message', `user-${turn}`, { role: 'user', text: `Prompt ${turn} seed ${seed}` }),
    }))
    events.push(event(session, cursor++, turn, 'item.completed', {
      item: providerItem('message', `assistant-${turn}`, { role: 'assistant', text: `Answer ${turn} ${sha256Bytes(`${seed}:${turn}`).slice(0, 12)}` }),
    }))
  }
  return {
    name: `items-${itemCount / 1000}k`,
    category: 'valid',
    description: `${itemCount} display items in one selected conversation for render/windowing stress.`,
    eventStream: events,
    operations: [
      operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 }),
      operation('pageResponse', 10, { session_id: session, session_generation: 1, selection_epoch: 1, event_count: events.length }),
    ],
    expectations: {
      selectedConversationItemCount: itemCount,
      totalItemsAreInOneConversation: true,
      replayable: true,
    },
  }
}

function buildConversationSwitchScenario(seed) {
  const events = []
  const operations = []
  for (let sessionIndex = 0; sessionIndex < 20; ++sessionIndex) {
    let cursor = 1
    const session = `conv-${sessionIndex.toString().padStart(2, '0')}`
    events.push(event(session, cursor++, 1, 'item.completed', {
      item: providerItem('message', `${session}-user`, { role: 'user', text: `Conversation ${sessionIndex} seed ${seed}` }),
    }))
    events.push(event(session, cursor++, 1, 'item.completed', {
      item: providerItem('message', `${session}-assistant`, { role: 'assistant', text: `Conversation ${sessionIndex} response` }),
    }))
  }
  const rng = rngFrom(seed)
  for (let index = 0; index < 100; ++index) {
    const sessionIndex = Math.floor(rng() * 20)
    operations.push(operation('selectConversation', index * 35, {
      session_id: `conv-${sessionIndex.toString().padStart(2, '0')}`,
      session_generation: 1,
      selection_epoch: index + 1,
    }))
  }
  return {
    name: '20-conversations-100-switches',
    category: 'valid',
    description: 'Twenty conversations plus one hundred deterministic selection operations.',
    eventStream: events,
    operations,
    expectations: { conversationCount: 20, selectionOperationCount: 100, replayable: true },
  }
}

function buildGiantTurnScenario(seed) {
  const session = 'giant-turn-session'
  const events = []
  let cursor = 1
  events.push(event(session, cursor++, 1, 'item.started', {
    item: providerItem('message', 'giant-assistant', { role: 'assistant' }),
  }))
  for (let index = 0; index < 512; ++index) {
    events.push(event(session, cursor++, 1, 'assistant.delta', {
      item_id: 'giant-assistant',
      delta_index: index,
      text_delta: `delta-${index.toString().padStart(3, '0')} ${sha256Bytes(`${seed}:giant:${index}`).slice(0, 24)}\n`,
    }))
  }
  events.push(event(session, cursor++, 1, 'item.completed', {
    item: providerItem('message', 'giant-assistant', {
      role: 'assistant',
      text: textOfSize(`giant ${seed}`, giantSourceBytes),
    }),
  }))
  return {
    name: 'giant-turn',
    category: 'valid',
    description: 'One assistant turn with more than 500 events and one >=8 MiB final item body.',
    eventStream: events,
    operations: [
      operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 }),
      ...events.map((value, index) => operation('liveEvent', index * 20, { session_id: session, session_generation: 1, selection_epoch: 1, cursor: value.cursor })),
    ],
    expectations: {
      minEventsInTurn: 500,
      minFinalItemUtf8Bytes: 8 * 1024 * 1024,
      replayable: true,
    },
  }
}

function buildMultiAssistantToolScenario(seed) {
  const session = 'multi-assistant-tool'
  const longCommand = `python scripts/build_report.py --session ${session} --seed ${seed} --include-tool-output --write-json --no-truncate`
  const events = [
    event(session, 1, 1, 'item.completed', {
      item: providerItem('message', 'user-1', { role: 'user', text: 'Run analysis and explain the result.' }),
    }),
    event(session, 2, 1, 'item.started', {
      item: providerItem('command', 'cmd-1', { command: longCommand, status: 'running' }),
    }),
    event(session, 3, 1, 'item.completed', {
      item: providerItem('message', 'assistant-a', { role: 'assistant', text: 'I will run the report first.' }),
    }),
    event(session, 4, 1, 'item.completed', {
      item: providerItem('tool_result', 'tool-1', { tool: 'analysis.build_report', text: 'report.json written' }),
    }),
    event(session, 5, 1, 'item.completed', {
      item: providerItem('message', 'assistant-b', { role: 'assistant', text: 'The report completed and the summary follows.' }),
    }),
    event(session, 6, 1, 'item.completed', {
      item: providerItem('command', 'cmd-1', { command: longCommand, status: 'completed', exitCode: 0 }),
    }),
  ]
  return {
    name: 'multi-assistant-tool-items',
    category: 'valid',
    description: 'Multiple assistant items and a tool item inside one turn.',
    eventStream: events,
    operations: [operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 })],
    expectations: {
      assistantItemsInTurnOne: ['assistant-a', 'assistant-b'],
      toolItemsInTurnOne: ['tool-1'],
      replayable: true,
    },
  }
}

function buildOverlapScenario(seed) {
  const session = 'overlap-duplicates-stale'
  const events = []
  for (let index = 0; index < 50; ++index) {
    events.push(event(session, index + 1, 1, 'assistant.delta', {
      item_id: 'stream-item',
      delta_index: index,
      text_delta: `stream ${index} ${sha256Bytes(`${seed}:stream:${index}`).slice(0, 8)}\n`,
    }))
  }
  const operations = [
    operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 }),
    ...events.map((value, index) => operation('liveEvent', index * 20, {
      session_id: session,
      session_generation: 1,
      selection_epoch: 1,
      cursor: value.cursor,
      delivery_generation: 1,
    })),
    operation('pageResponse', 250, {
      session_id: session,
      session_generation: 1,
      selection_epoch: 1,
      response_id: 'page-valid-overlap',
      cursor_range: [20, 40],
      contains_duplicate_cursors: true,
      duplicate_policy: 'same-event-is-idempotent',
    }),
    operation('selectConversation', 900, { session_id: 'other-session', session_generation: 1, selection_epoch: 2 }),
    operation('pageResponse', 960, {
      session_id: session,
      session_generation: 1,
      selection_epoch: 1,
      current_selection_epoch: 2,
      response_id: 'page-stale-selection',
      cursor_range: [41, 50],
      expected_consumer_action: 'drop-stale-response',
    }),
  ]
  return {
    name: 'stream-overlap-duplicates-stale',
    category: 'valid',
    description: '50 deltas/s schedule with page/live overlap, idempotent duplicates, and stale selection response context.',
    eventStream: events,
    operations,
    expectations: {
      liveRatePerSecond: 50,
      duplicateCursorRange: [20, 40],
      staleResponseHasSelectionEpoch: true,
      replayable: true,
    },
  }
}

function buildRichMarkdownScenario(seed) {
  const session = 'rich-markdown-media-command'
  const command = `ket render --input transcript.md --seed ${seed} --format markdown-table-mermaid-media --status-json`
  const markdown = [
    '# Transcript Fixture',
    '',
    '| feature | status |',
    '| --- | --- |',
    '| code | ok |',
    '| media | ok |',
    '',
    '```ts',
    'export const answer = 42',
    '```',
    '',
    '```mermaid',
    'sequenceDiagram',
    '  participant U as User',
    '  participant A as Assistant',
    '  U->>A: render',
    '```',
    '',
    '![fixture media](fixture://media/rich-markdown.png)',
  ].join('\n')
  const events = [
    event(session, 1, 1, 'item.completed', {
      item: providerItem('message', 'user-rich', { role: 'user', text: 'Render the rich transcript.' }),
    }),
    event(session, 2, 1, 'item.started', {
      item: providerItem('command', 'cmd-rich', { command, status: 'running' }),
    }),
    event(session, 3, 1, 'item.completed', {
      item: providerItem('media', 'media-rich', { mime: 'image/png', uri: 'fixture://media/rich-markdown.png', alt: 'Synthetic chart' }),
    }),
    event(session, 4, 1, 'item.completed', {
      item: providerItem('message', 'assistant-rich', { role: 'assistant', text: markdown }),
    }),
    event(session, 5, 1, 'item.completed', {
      item: providerItem('command', 'cmd-rich', { command, status: 'completed', exitCode: 0 }),
    }),
  ]
  return {
    name: 'rich-markdown-media-command',
    category: 'valid',
    description: 'Markdown/code/table/Mermaid/media plus command lifecycle.',
    eventStream: events,
    operations: [operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 })],
    expectations: { includesMarkdown: true, includesMermaid: true, includesMedia: true, mediaCoverage: 'reference parsing only; no asset resolver, decode or render measurement', commandLifecycle: true, replayable: true },
  }
}

function buildCommandOutputScenario(seed) {
  const session = 'command-output-cloud-strip-marker'
  const command = `python scripts/deploy_report.py --workspace transcript-refactor --seed ${seed} --include-all-command-output-fields --long-argument-for-no-truncation-check 0123456789abcdefghijklmnopqrstuvwxyz`
  const events = [
    event(session, 1, 1, 'item.started', {
      item: providerItem('command', 'cmd-output', { command, status: 'running' }),
    }),
    event(session, 2, 1, 'command.output_delta', {
      item_id: 'cmd-output',
      stream: 'stdout',
      text: `${commandOutputMarker}: line 1\n`,
    }),
    event(session, 3, 1, 'command.output_aggregated', {
      item_id: 'cmd-output',
      stdout: `${aggregatedOutputMarker}: aggregated stdout\n`,
      stderr: '',
    }),
    event(session, 4, 1, 'item.completed', {
      item: providerItem('command', 'cmd-output', { command, status: 'completed', exitCode: 0 }),
    }),
  ]
  return {
    name: 'command-output-cloud-strip-marker',
    category: 'valid',
    description: 'Raw command output marker events plus expected command-only cloud projection.',
    eventStream: events,
    operations: [operation('selectConversation', 0, { session_id: session, session_generation: 1, selection_epoch: 1 })],
    expected: {
      cloudProjectionEvents: commandScenarioExpected(events),
      forbiddenCloudMarkers: [commandOutputMarker, aggregatedOutputMarker],
      requiredCommandText: command,
      requiredStatus: 'completed',
    },
    expectations: {
      commandOutputMarkersExistInRawFixture: true,
      expectedCloudProjectionDropsOutput: true,
      commandTextMustNotBeTruncated: true,
      replayable: true,
    },
  }
}

function buildNegativeScenarios(seed) {
  return [
    {
      name: 'negative-cursor-gap',
      category: 'negative',
      description: 'Intentional gap in a raw stream explicitly requiring contiguous cursors; compact/cloud streams may have gaps.',
      eventStream: [
        event('negative-gap', 1, 1, 'item.completed', { item: providerItem('message', 'n1', { role: 'user', text: seed }) }),
        event('negative-gap', 3, 1, 'item.completed', { item: providerItem('message', 'n2', { role: 'assistant', text: 'gap after cursor 1' }) }),
      ],
      operations: [operation('pageResponse', 0, { session_id: 'negative-gap', session_generation: 1, cursor_range: [1, 3] })],
      expectations: { expectedFailure: 'cursor_gap', requiresContiguousRawStream: true, replayable: false },
    },
    {
      name: 'negative-conflicting-duplicate',
      category: 'negative',
      description: 'Same cursor with different payload; consumers should reject conflict.',
      eventStream: [
        event('negative-duplicate', 1, 1, 'item.completed', { item: providerItem('message', 'dup-a', { role: 'assistant', text: seed }) }),
        event('negative-duplicate', 1, 1, 'item.completed', { item: providerItem('message', 'dup-b', { role: 'assistant', text: 'conflicting duplicate' }) }),
      ],
      operations: [operation('pageResponse', 0, { session_id: 'negative-duplicate', session_generation: 1, contains_conflicting_duplicate: true })],
      expectations: { expectedFailure: 'conflicting_duplicate_cursor', replayable: false },
    },
    {
      name: 'negative-stale-generation-response',
      category: 'negative',
      description: 'Page response is for stale session generation and stale selection epoch.',
      eventStream: [
        event('negative-stale', 1, 1, 'item.completed', {
          item: providerItem('message', 'stale-a', { role: 'assistant', text: 'generation 1 response' }),
        }, { session_generation: 1 }),
      ],
      operations: [
        operation('selectConversation', 0, { session_id: 'negative-stale', session_generation: 2, selection_epoch: 2 }),
        operation('pageResponse', 50, {
          session_id: 'negative-stale',
          session_generation: 1,
          selection_epoch: 1,
          current_session_generation: 2,
          current_selection_epoch: 2,
          expected_consumer_action: 'reject-stale-generation',
        }),
      ],
      expectations: { expectedFailure: 'stale_generation_response', replayable: false },
    },
  ]
}

export function scenarios(seed = defaultSeed) {
  return [
    buildItemsScenario(seed, 1000),
    buildItemsScenario(seed, 10000),
    buildConversationSwitchScenario(seed),
    buildGiantTurnScenario(seed),
    buildMultiAssistantToolScenario(seed),
    buildOverlapScenario(seed),
    buildRichMarkdownScenario(seed),
    buildCommandOutputScenario(seed),
    ...buildNegativeScenarios(seed),
  ].map((scenario) => {
    scenario.operations = scenario.operations.map((op, index) => ({ ...op, operation_ordinal: index }))
      .sort((a, b) => a.offset_ms - b.offset_ms || a.operation_ordinal - b.operation_ordinal)
    for (const op of scenario.operations) {
      const matches = scenario.eventStream.filter((e) => e.session_id === op.session_id && e.session_generation === op.session_generation)
      if (op.kind === 'liveEvent' && !matches.some((e) => e.cursor === op.cursor)) throw new Error('Unresolved live event reference')
      if (op.kind === 'pageResponse' && scenario.category === 'valid') {
        if (!matches.length || (op.event_count !== undefined && op.event_count !== matches.length)) throw new Error('Unresolved page reference')
        if (op.cursor_range && !op.cursor_range.every((cursor) => matches.some((e) => e.cursor === cursor))) throw new Error('Unresolved page range')
      }
    }
    return scenario
  })
}

function selectScenarios(seed, scenarioName) {
  const all = scenarios(seed)
  if (scenarioName === 'all') return all
  const selected = all.find((scenario) => scenario.name === scenarioName)
  if (!selected) throw new Error(`Unknown scenario: ${scenarioName}`)
  return [selected]
}

function statsFor(records, lineFactory, representation) {
  const acc = hashAccumulator()
  for (const record of records) acc.add(lineFactory(record))
  return acc.digest(representation)
}

function scenarioManifest(scenario) {
  const eventCanonical = statsFor(scenario.eventStream, canonicalLine, 'logical canonical event JSON; sorted keys; one trailing newline per record')
  const eventJsonl = statsFor(scenario.eventStream, jsonlLine, 'exact materialized event JSONL bytes; JSON.stringify(record) plus \\n')
  const operationsJsonl = statsFor(scenario.operations, jsonlLine, 'exact materialized operation JSONL bytes; JSON.stringify(record) plus \\n')
  const cloudProjection = scenario.expected?.cloudProjectionEvents ?? scenario.eventStream.map(projectForCloud).filter(Boolean)
  const cloudProjectionJsonl = statsFor(cloudProjection, jsonlLine, 'generator-local expected cloud projection JSONL; command output events omitted')
  const sessions = new Set(scenario.eventStream.map((value) => value.session_id))
  const turns = new Set(scenario.eventStream.map((value) => `${value.session_id}:${value.session_generation}:${value.turn_sequence}`))
  const specificationHash = sha256Bytes(stableJson({
    name: scenario.name,
    category: scenario.category,
    description: scenario.description,
    expectations: scenario.expectations,
    expected: scenario.expected ?? null,
  }))
  return {
    name: scenario.name,
    category: scenario.category,
    description: scenario.description,
    eventCount: scenario.eventStream.length,
    operationCount: scenario.operations.length,
    sessionCount: sessions.size,
    turnCount: turns.size,
    specificationHash,
    logicalCanonical: eventCanonical,
    exactJsonl: eventJsonl,
    operationsJsonl,
    cloudProjection: {
      ...cloudProjectionJsonl,
      containsOutputMarker: JSON.stringify(cloudProjection).includes(commandOutputMarker),
    },
    expectations: scenario.expectations,
    files: {
      events: `${scenario.name}.events.jsonl`,
      operations: `${scenario.name}.operations.jsonl`,
      expected: `${scenario.name}.expected.json`,
    },
  }
}

export function buildManifest(seed = defaultSeed, scenarioName = 'all') {
  const selected = selectScenarios(seed, scenarioName)
  return {
    version: fixtureVersion,
    seed,
    generatedAt: 'deterministic-manifest-v3',
    scope: 'workload fixtures only; not a canonical wire contract',
    replaySemantics: {
      cursorNamespace: '(session_id, session_generation); never a global ordinal',
      order: 'offset_ms ascending, then operation_ordinal ascending',
      pageReferences: 'cursor_range is inclusive in the named session/generation; missing range means all matching events',
      selections: 'selectConversation declares a selectable session/generation, including empty sessions',
      gaps: 'only negative-cursor-gap requires contiguous raw cursors; compact/cloud gaps are allowed',
      media: 'reference parsing only; decode/render not_measured',
    },
    hashNotes: [
      'specificationHash covers scenario metadata and expectations, not materialized payload bytes.',
      'logicalCanonical hashes sorted-key JSON event records with one trailing newline per record.',
      'exactJsonl hashes JSON.stringify(record) plus newline, matching materialized event files byte-for-byte.',
      'operationsJsonl hashes workload operation JSONL separately from transcript events.',
    ],
    scenarios: selected.map(scenarioManifest),
  }
}

async function writeJsonl(path, records) {
  const stream = createWriteStream(path, { encoding: 'utf8' })
  for (const record of records) {
    if (!stream.write(jsonlLine(record))) await once(stream, 'drain')
  }
  stream.end()
  await once(stream, 'finish')
}

export async function materialize(seed = defaultSeed, scenarioName = 'all', directory = '') {
  if (!directory) throw new Error('materialize directory is required')
  const target = resolve(directory)
  mkdirSync(target, { recursive: true })
  const selected = selectScenarios(seed, scenarioName)
  for (const scenario of selected) {
    await writeJsonl(join(target, `${scenario.name}.events.jsonl`), scenario.eventStream)
    await writeJsonl(join(target, `${scenario.name}.operations.jsonl`), scenario.operations)
    const expected = {
      name: scenario.name,
      category: scenario.category,
      expectations: scenario.expectations,
      expected: scenario.expected ?? {
        cloudProjectionEvents: scenario.eventStream.map(projectForCloud).filter(Boolean),
      },
    }
    writeFileSync(join(target, `${scenario.name}.expected.json`), `${JSON.stringify(expected, null, 2)}\n`)
  }
  const manifest = buildManifest(seed, scenarioName)
  writeFileSync(join(target, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`)
  return manifest
}

export function selfTest(seed = defaultSeed) {
  const first = buildManifest(seed, 'all')
  const second = buildManifest(seed, 'all')
  if (JSON.stringify(first) !== JSON.stringify(second)) throw new Error('Manifest is not deterministic for the same seed')
  const third = buildManifest(`${seed}:changed`, 'all')
  if (first.scenarios[0].exactJsonl.sha256 === third.scenarios[0].exactJsonl.sha256) throw new Error('Payload hash did not change when seed changed')
  const giant = scenarios(seed).find((scenario) => scenario.name === 'giant-turn')
  const finalItem = giant.eventStream.at(-1).payload.item
  if (giant.eventStream.length <= 500) throw new Error('giant-turn does not contain more than 500 events')
  if (Buffer.byteLength(finalItem.text) < 8 * 1024 * 1024) throw new Error('giant-turn final item is smaller than 8 MiB')
  const commandScenario = scenarios(seed).find((scenario) => scenario.name === 'command-output-cloud-strip-marker')
  const raw = JSON.stringify(commandScenario.eventStream)
  const projected = JSON.stringify(commandScenario.expected.cloudProjectionEvents)
  if (!raw.includes(commandOutputMarker) || !raw.includes(aggregatedOutputMarker)) throw new Error('Raw command output markers are missing')
  if (projected.includes(commandOutputMarker) || projected.includes(aggregatedOutputMarker)) throw new Error('Expected cloud projection contains command output markers')
  if (!projected.includes(commandScenario.expected.requiredCommandText)) throw new Error('Expected projection does not retain full command text')
  return true
}

async function main() {
  const args = parseArgs(process.argv)
  if (args.help) {
    process.stdout.write(usage())
    return
  }
  if (args.list) {
    process.stdout.write(`${scenarios(args.seed).map((scenario) => scenario.name).join('\n')}\n`)
    return
  }
  if (args.selfTest) selfTest(args.seed)
  const manifest = buildManifest(args.seed, args.scenario)
  if (args.manifest) {
    const manifestPath = resolve(args.manifest)
    mkdirSync(dirname(manifestPath), { recursive: true })
    writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`)
  }
  if (args.materialize) await materialize(args.seed, args.scenario, args.materialize)
  if (!args.manifest && !args.materialize && !args.selfTest) process.stdout.write(`${JSON.stringify(manifest, null, 2)}\n`)
}

if (import.meta.url === `file://${process.argv[1]}`) {
  main().catch((error) => {
    process.stderr.write(`${error.message}\n`)
    process.exit(1)
  })
}
