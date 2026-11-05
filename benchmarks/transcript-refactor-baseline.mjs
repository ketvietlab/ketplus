#!/usr/bin/env node

import { execFileSync, spawnSync } from 'node:child_process'
import { mkdirSync, writeFileSync, readFileSync, mkdtempSync, copyFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { isDeepStrictEqual } from 'node:util'
import { buildManifest } from '../tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs'
import { dirname, resolve } from 'node:path'
import process from 'node:process'

function usage() {
  return `Usage:
  node benchmarks/transcript-refactor-baseline.mjs [options]

Options:
  --out <path>                    Write diagnostic report JSON.
  --seed <value>                  Fixture seed. Default: ketplus-transcript-refactor-wave1
  --manifest <path>               Fixture manifest path.
  --generator-script <path>       Fixture generator script.
  --run-daemon-restore            Run legacy daemon restore benchmark after fixture generation succeeds.
  --daemon-restore-script <path>  Restore benchmark script. Default: benchmarks/run-daemon-restore.sh
  --native-capture-granted        Record that permission was granted; does not mark capture measured.
  --help                          Show this help.

Unsupported:
  --release-build-dir             Rejected here because run-daemon-restore.sh owns its build directory.
`
}

function argValue(flag, fallback = '') {
  const index = process.argv.indexOf(flag)
  return index >= 0 ? process.argv[index + 1] ?? fallback : fallback
}

function hasFlag(flag) {
  return process.argv.includes(flag)
}

function failArg(message) {
  const error = new Error(message)
  error.argumentError = true
  throw error
}

function validateArgs() {
  if (hasFlag('--help') || hasFlag('-h')) return { help: true }
  const supported = new Set([
    '--out',
    '--seed',
    '--manifest',
    '--generator-script',
    '--run-daemon-restore',
    '--daemon-restore-script',
    '--native-capture-granted',
  ])
  const valueFlags = new Set(['--out', '--seed', '--manifest', '--generator-script', '--daemon-restore-script'])
  for (let index = 2; index < process.argv.length; ++index) {
    const arg = process.argv[index]
    if (arg === '--release-build-dir') failArg('--release-build-dir is not supported by this isolated harness; use run-daemon-restore.sh directly until ownership is agreed.')
    if (!supported.has(arg)) failArg(`Unknown argument: ${arg}`)
    if (valueFlags.has(arg)) {
      const value = process.argv[++index]
      if (value === undefined || value.startsWith('--')) failArg(`${arg} requires a value`)
    }
  }
  return {
    out: argValue('--out', `benchmarks/results/raw-${new Date().toISOString().replaceAll(/[-:]/g, '').replace(/\..*/, 'Z')}-transcript-refactor-baseline.json`),
    seed: argValue('--seed', 'ketplus-transcript-refactor-wave1'),
    manifest: argValue('--manifest', 'benchmarks/results/transcript-refactor-fixture-manifest.json'),
    generatorScript: argValue('--generator-script', 'tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs'),
    daemonRestoreScript: argValue('--daemon-restore-script', 'benchmarks/run-daemon-restore.sh'),
    runDaemonRestore: hasFlag('--run-daemon-restore'),
    nativeCaptureGranted: hasFlag('--native-capture-granted'),
  }
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { encoding: 'utf8', ...options })
  if (result.error) {
    return {
      requested: true,
      status: 'failed',
      command: [command, ...args].join(' '),
      exitCode: null,
      signal: null,
      error: result.error.message,
      stdout: result.stdout ?? '',
      stderr: result.stderr ?? '',
    }
  }
  return {
    requested: true,
    status: result.status === 0 ? 'completed' : 'failed',
    command: [command, ...args].join(' '),
    exitCode: result.status,
    signal: result.signal,
    stdout: result.stdout ?? '',
    stderr: result.stderr ?? '',
  }
}

function git(args, fallback = '') {
  try {
    return execFileSync('git', args, { encoding: 'utf8' }).trim()
  } catch {
    return fallback
  }
}

function writeJson(path, value) {
  const target = resolve(path)
  mkdirSync(dirname(target), { recursive: true })
  writeFileSync(target, `${JSON.stringify(value, null, 2)}\n`)
}

function baseReport(args) {
  return {
    reportVersion: 2,
    generatedAt: new Date().toISOString(),
    repository: git(['rev-parse', '--show-toplevel'], '.'),
    head: git(['rev-parse', 'HEAD'], 'unknown'),
    branch: git(['rev-parse', '--abbrev-ref', 'HEAD'], 'unknown'),
    environment: {
      os: run('uname', ['-srvmp']).stdout.trim(),
      node: process.version,
      qtRoot: process.env.QT_ROOT ?? '',
    },
    fixture: {
      seed: args.seed,
      manifestPath: args.manifest,
      generation: {
        requested: true,
        status: 'not_run',
      },
    },
    measurementLimits: {
      nativeCaptureGranted: args.nativeCaptureGranted,
      nativeViewportCaptureImplemented: false,
      offscreenFpsUsedAsNativeEvidence: false,
      providerQuotaUsed: false,
      userJournalUsed: false,
      notes: [
        'This harness records raw evidence and limitations; it does not infer p99 from a few samples.',
        'Native capture permission does not create a measurement without a capture implementation and artifact.',
        'Legacy daemon restore benchmark uses its built-in 300-turn synthetic journal, not the transcript-refactor workload fixtures.',
      ],
    },
    metrics: {
      daemonRestore: args.runDaemonRestore
        ? { requested: true, status: 'not_run', reason: 'fixture generation has not completed yet' }
        : { requested: false, status: 'not_run', reason: 'Use --run-daemon-restore to request legacy daemon restore measurement.' },
      nativeViewportCapture: {
        requested: args.nativeCaptureGranted,
        status: 'not_measured',
        reason: 'No native capture implementation/artifact is wired in this isolated harness.',
      },
    },
  }
}

function runHarness(args) {
  const report = baseReport(args)
  if (resolve(args.out) === resolve(args.manifest)) {
    report.fixture.generation = { requested: false, status: 'failed', error: 'report/manifest path collision' }
    return { report, exitCode: 2 }
  }
  // A fresh output path prevents an old manifest from satisfying this invocation.
  const freshManifest = resolve(mkdtempSync(`${tmpdir()}/ketplus-baseline-`), 'manifest.json')
  const generator = run(process.execPath, [
    args.generatorScript,
    '--seed',
    args.seed,
    '--manifest',
    freshManifest,
  ])
  report.fixture.generation = {
    ...generator,
    subprocessStatus: generator.status,
    artifactValidation: 'not_run',
    artifactPath: null,
  }

  try {
    const manifest = JSON.parse(readFileSync(freshManifest, 'utf8'))
    if (!isDeepStrictEqual(manifest, buildManifest(args.seed))) throw new Error('Manifest version/seed/schema/integrity does not match the deterministic workload specification')
    mkdirSync(dirname(resolve(args.manifest)), { recursive: true })
    copyFileSync(freshManifest, args.manifest)
    report.fixture.generation.artifactValidation = 'validated'
    report.fixture.generation.artifactPath = args.manifest
  } catch (error) {
    Object.assign(report.fixture.generation, { status: 'failed', artifactValidation: 'failed', error: error.message })
    report.metrics.daemonRestore.reason = 'fixture artifact validation failed; benchmark skipped'
    return { report, exitCode: 1 }
  }
  if (generator.status !== 'completed') {
    if (args.runDaemonRestore) report.metrics.daemonRestore = { requested: true, status: 'not_run', reason: 'fixture generation failed; benchmark skipped' }
    return { report, exitCode: generator.exitCode ?? 1 }
  }

  if (args.runDaemonRestore) {
    const output = `benchmarks/results/raw-${Date.now()}-daemon-restore-transcript-refactor.txt`
    const restore = run(args.daemonRestoreScript, [output], { env: process.env })
    report.metrics.daemonRestore = {
      ...restore,
      requested: true,
      rawOutputPath: restore.status === 'completed' ? output : null,
      fixtureUsed: 'legacy built-in 300-turn synthetic journal from run-daemon-restore.sh',
    }
    if (restore.status !== 'completed') return { report, exitCode: restore.exitCode ?? 1 }
  }

  return { report, exitCode: 0 }
}

let args
try {
  args = validateArgs()
  if (args.help) {
    process.stdout.write(usage())
    process.exit(0)
  }
} catch (error) {
  const out = argValue('--out', '')
  if (out) {
    writeJson(out, {
      reportVersion: 2,
      generatedAt: new Date().toISOString(),
      fixture: { generation: { requested: false, status: 'failed', error: error.message } },
      metrics: {
        daemonRestore: { requested: hasFlag('--run-daemon-restore'), status: 'not_run', reason: 'argument validation failed' },
        nativeViewportCapture: { requested: hasFlag('--native-capture-granted'), status: 'not_measured', reason: 'argument validation failed' },
      },
    })
  }
  process.stderr.write(`${error.message}\n`)
  process.exit(error.argumentError ? 2 : 1)
}

const { report, exitCode } = runHarness(args)
writeJson(args.out, report)
process.stdout.write(`wrote ${args.out}\n`)
process.exit(exitCode)
