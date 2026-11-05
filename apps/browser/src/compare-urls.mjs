#!/usr/bin/env node
import path from "node:path";
import { BrowserService } from "./browser-service.mjs";
import { safeUrl } from "./origin-policy.mjs";

const [referenceUrl, candidateUrl] = process.argv.slice(2);
if (!referenceUrl || !candidateUrl) {
  process.stderr.write("Usage: npm run compare -- <reference-url> <candidate-url>\n");
  process.exit(2);
}

let referenceOrigin;
let candidateOrigin;
try {
  referenceOrigin = new URL(referenceUrl).origin;
  candidateOrigin = new URL(candidateUrl).origin;
} catch {
  process.stderr.write("Both inputs must be absolute HTTP(S) URLs.\n");
  process.exit(2);
}

// The command line is the host here: it grants exactly the two input origins.
const service = new BrowserService({
  artifactsDir: path.resolve("artifacts"),
  allowedOrigins: [referenceOrigin, candidateOrigin],
  retainArtifacts: true,
});
try {
  const environment = {
    viewport: { width: 1440, height: 900 },
    deviceScaleFactor: 1,
    theme: "light",
    locale: "en-US",
    reducedMotion: "reduce",
  };
  const referenceSession = await service.createSession({
    ...environment,
    allowedOrigins: [referenceOrigin],
    evidenceContext: { role: "reference", sourceUrl: safeUrl(referenceUrl) },
  });
  const candidateSession = await service.createSession({
    ...environment,
    allowedOrigins: [candidateOrigin],
    evidenceContext: { role: "candidate", sourceUrl: safeUrl(candidateUrl) },
  });
  const referencePage = await service.open({ sessionId: referenceSession.sessionId, url: referenceUrl });
  const candidatePage = await service.open({ sessionId: candidateSession.sessionId, url: candidateUrl });
  const reference = await service.observe({
    sessionId: referenceSession.sessionId,
    pageId: referencePage.pageId,
    includeScreenshot: true,
    label: "reference",
  });
  const candidate = await service.observe({
    sessionId: candidateSession.sessionId,
    pageId: candidatePage.pageId,
    includeScreenshot: true,
    label: "candidate",
  });
  const comparison = await service.compare({
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { snapshotId: candidate.snapshotId, captureId: candidate.captureId },
    label: "url-comparison",
  });
  process.stdout.write(`${JSON.stringify(comparison, null, 2)}\n`);
  process.exitCode = comparison.status === "passed" ? 0 : 1;
} finally {
  await service.close();
}
