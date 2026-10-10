#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..", "..");
const siteRoot = path.join(repositoryRoot, "site");
const timeoutMs = 15_000;
const concurrency = 5;

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const target = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(target) : [target];
  });
}

const references = new Map();
const sourceFiles = [
  ...walk(siteRoot).filter((candidate) => candidate.endsWith(".html")),
  path.join(repositoryRoot, "README.md"),
  path.join(repositoryRoot, "SUPPORT.md"),
  path.join(repositoryRoot, "ACKNOWLEDGEMENTS.md"),
  ...walk(path.join(repositoryRoot, "docs", "launch")).filter((candidate) => candidate.endsWith(".md")),
];

function addReference(rawReference, file) {
  const decoded = rawReference.replaceAll("&amp;", "&").trim();
  const url = new URL(decoded);
  if (url.protocol !== "https:") {
    throw new Error(`${path.relative(repositoryRoot, file)}: public external link is not HTTPS: ${decoded}`);
  }
  if (url.username || url.password) {
    throw new Error(`${path.relative(repositoryRoot, file)}: public external link contains credentials: ${decoded}`);
  }
  url.hash = "";
  const key = url.href;
  const source = path.relative(repositoryRoot, file);
  if (!references.has(key)) references.set(key, new Set());
  references.get(key).add(source);
}

for (const file of sourceFiles) {
  const text = fs.readFileSync(file, "utf8");
  const candidates = [
    ...text.matchAll(/\b(?:href|src|content)=["'](https?:\/\/[^"']+)["']/gi),
    ...text.matchAll(/!?\[[^\]]*\]\((https?:\/\/[^)\s]+)\)/gi),
    ...text.matchAll(/<(https?:\/\/[^>\s]+)>/gi),
  ];
  for (const match of candidates) addReference(match[1], file);
}

const urls = [...references.keys()].sort();
const failures = [];
const warnings = [];
let nextIndex = 0;

async function probe(url) {
  const response = await fetch(url, {
    method: "GET",
    redirect: "follow",
    headers: {
      "User-Agent": "Eclipse-Umbra-link-check/1.0",
      Accept: "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.1",
      Range: "bytes=0-0",
    },
    signal: AbortSignal.timeout(timeoutMs),
  });
  await response.body?.cancel();
  return response;
}

async function worker() {
  while (true) {
    const index = nextIndex;
    nextIndex += 1;
    if (index >= urls.length) return;

    const url = urls[index];
    const sourceList = [...references.get(url)].join(", ");
    try {
      const response = await probe(url);
      if (response.status >= 200 && response.status < 400) continue;
      if ([401, 403, 405, 429].includes(response.status)) {
        warnings.push(`${url} returned ${response.status}; verify manually (${sourceList})`);
        continue;
      }
      failures.push(`${url} returned ${response.status} (${sourceList})`);
    } catch (error) {
      failures.push(`${url} could not be checked: ${error.message} (${sourceList})`);
    }
  }
}

await Promise.all(Array.from({ length: Math.min(concurrency, urls.length) }, () => worker()));

for (const warning of warnings.sort()) console.warn(`Warning: ${warning}`);
if (failures.length > 0) {
  console.error(`External-link check failed: ${failures.length} of ${urls.length} unique URLs need attention.`);
  for (const failure of failures.sort()) console.error(`- ${failure}`);
  process.exit(1);
}

console.log(`External-link check passed: ${urls.length} unique URLs (${warnings.length} manual warning(s)).`);
