#!/usr/bin/env node
// Reduces `codex app-server generate-json-schema --experimental` output to the protocol surface
// KetPlus checks (ADR 0015): every method with the field paths of its params and response.
//
//   codex app-server generate-json-schema --experimental --out /tmp/codex-schema
//   node tests/providers/codex/extract-surface.mjs /tmp/codex-schema <version> \
//     > tests/providers/codex/schemas/<version>/surface.json
import fs from "node:fs";
import path from "node:path";

const maximumDepth = 4;

export function extractSurface(bundle, version) {
  const definitions = bundle.definitions ?? {};
  const resolve = (schema) => {
    let current = schema;
    const seen = new Set();
    while (current && current.$ref && !seen.has(current.$ref)) {
      seen.add(current.$ref);
      current = definitions[current.$ref.replace("#/definitions/", "")];
    }
    return current ?? {};
  };
  const collect = (schema, prefix, depth, stack, out) => {
    const resolved = resolve(schema);
    const reference = schema?.$ref ?? null;
    if (reference && stack.includes(reference)) return;
    const nextStack = reference ? [...stack, reference] : stack;
    for (const variant of [...(resolved.oneOf ?? []), ...(resolved.anyOf ?? []), ...(resolved.allOf ?? [])])
      collect(variant, prefix, depth, nextStack, out);
    const types = [].concat(resolved.type ?? []);
    if (types.includes("array") && resolved.items) {
      collect(resolved.items, `${prefix}[]`, depth, nextStack, out);
      return;
    }
    if (depth >= maximumDepth) return;
    for (const [name, value] of Object.entries(resolved.properties ?? {})) {
      const field = prefix ? `${prefix}.${name}` : name;
      out.add(field);
      collect(value, field, depth + 1, nextStack, out);
    }
    if (resolved.additionalProperties && typeof resolved.additionalProperties === "object")
      collect(resolved.additionalProperties, `${prefix}{}`, depth, nextStack, out);
  };
  const fields = (schema) => {
    if (!schema) return [];
    const out = new Set();
    collect(schema, "", 0, [], out);
    return [...out].sort();
  };
  const methods = (union, withResponse) => {
    const result = {};
    for (const variant of resolve({ $ref: `#/definitions/${union}` }).oneOf ?? []) {
      const method = variant.properties?.method?.enum?.[0];
      if (!method) continue;
      const params = variant.properties?.params;
      const entry = { params: fields(params) };
      if (withResponse && params?.$ref) {
        const responseName = params.$ref.replace("#/definitions/", "").replace(/Params$/, "Response");
        entry.response = definitions[responseName] ? fields({ $ref: `#/definitions/${responseName}` }) : [];
      }
      result[method] = entry;
    }
    return Object.fromEntries(Object.entries(result).sort(([a], [b]) => a.localeCompare(b)));
  };
  return {
    provider: "codex",
    version,
    requests: methods("ClientRequest", true),
    notifications: methods("ServerNotification", false),
    server_requests: methods("ServerRequest", true),
  };
}

function loadBundle(directory) {
  // The v2 bundle holds every definition; ServerRequest ships as its own file.
  const bundle = JSON.parse(
    fs.readFileSync(path.join(directory, "codex_app_server_protocol.v2.schemas.json"), "utf8"),
  );
  for (const extra of ["ServerRequest", "ClientRequest", "ServerNotification"]) {
    if (bundle.definitions[extra]) continue;
    const file = path.join(directory, `${extra}.json`);
    if (!fs.existsSync(file)) continue;
    const schema = JSON.parse(fs.readFileSync(file, "utf8"));
    Object.assign(bundle.definitions, schema.definitions ?? {}, { [extra]: schema });
  }
  return bundle;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [directory, version] = process.argv.slice(2);
  if (!directory || !version) {
    console.error("usage: extract-surface.mjs <schema-dir> <version>");
    process.exit(2);
  }
  process.stdout.write(`${JSON.stringify(extractSurface(loadBundle(directory), version), null, 1)}\n`);
}
