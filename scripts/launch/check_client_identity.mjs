#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..", "..");
const errors = [];

function read(relativePath) {
  return fs.readFileSync(path.join(repositoryRoot, relativePath), "utf8");
}

function requireMatch(relativePath, pattern, message) {
  if (!pattern.test(read(relativePath))) errors.push(`${relativePath}: ${message}`);
}

function rejectMatch(relativePath, pattern, message) {
  if (pattern.test(read(relativePath))) errors.push(`${relativePath}: ${message}`);
}

requireMatch("deploy/webos/appinfo.json", /"vendor": "Eclipse\/Umbra contributors"/, "suite vendor identity is missing");
requireMatch("deploy/webos/appinfo.json", /"appDescription": "Eclipse /, "visible description does not name Eclipse");
rejectMatch("deploy/webos/appinfo.json", /"(?:vendor|appDescription)": "[^"]*Aurora/, "visible Aurora branding remains");

const artworkManifestPath = path.join(repositoryRoot, "scripts", "launch", "assets", "webos-artwork.render.json");
if (!fs.existsSync(artworkManifestPath)) {
  errors.push("scripts/launch/assets/webos-artwork.render.json: reproducible webOS artwork manifest is missing");
} else {
  try {
    const manifest = JSON.parse(fs.readFileSync(artworkManifestPath, "utf8"));
    if (manifest.renderer !== "sharp 0.35.4") errors.push("webOS artwork renderer is not pinned at sharp 0.35.4");
    for (const item of [...(manifest.sources ?? []), ...(manifest.outputs ?? [])]) {
      const file = path.join(repositoryRoot, item.path);
      if (!fs.existsSync(file)) {
        errors.push(`${item.path}: webOS artwork source/output is missing`);
        continue;
      }
      const actualHash = crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
      if (actualHash !== item.sha256) errors.push(`${item.path}: webOS artwork hash is out of sync with its render manifest`);
      if (item.width || item.height) {
        const bytes = fs.readFileSync(file);
        if (bytes.length < 24 || bytes.toString("hex", 0, 8) !== "89504e470d0a1a0a") {
          errors.push(`${item.path}: rendered webOS artwork is not a PNG`);
        } else if (bytes.readUInt32BE(16) !== item.width || bytes.readUInt32BE(20) !== item.height) {
          errors.push(`${item.path}: rendered webOS artwork dimensions do not match the manifest`);
        }
      }
    }
  } catch (error) {
    errors.push(`scripts/launch/assets/webos-artwork.render.json: invalid manifest (${error.message})`);
  }
}

const generatedWebosManifest = "build/webos-easy_build/appinfo.json";
if (fs.existsSync(path.join(repositoryRoot, generatedWebosManifest))) {
  requireMatch(generatedWebosManifest, /"title": "Eclipse"/, "generated webOS title is not Eclipse");
  requireMatch(generatedWebosManifest, /"vendor": "Eclipse\/Umbra contributors"/, "generated webOS vendor is stale");
  rejectMatch(
    generatedWebosManifest,
    /"(?:vendor|appDescription)": "[^"]*Aurora/,
    "generated webOS artifact still contains visible Aurora branding; rebuild and inspect the IPK",
  );
}

requireMatch("deploy/steamlink/toc.txt", /^name=Eclipse$/m, "launcher name is not Eclipse");
requireMatch("deploy/webosbrew/com.aurora.gamestream.yml", /^title: Eclipse$/m, "listing title is not Eclipse");
requireMatch("deploy/linux/moonlight-tv.desktop", /^Name=Moonlight TV$/m, "inherited Moonlight TV name changed unexpectedly");

requireMatch("CMakeLists.txt", /set\(WEBOS_APPINFO_TITLE "Eclipse"\)/, "webOS live title is not Eclipse");
requireMatch("CMakeLists.txt", /set\(WEBOS_APPINFO_TITLE "Eclipse Beta"\)/, "webOS beta title is not Eclipse Beta");
requireMatch("src/app/platform/windows/eclipse.rc.in", /VALUE "ProductName", "Eclipse\\0"/, "Windows product metadata is not Eclipse");
requireMatch("src/app/app.c", /locstr\("Quit Eclipse\?"\)/, "in-app quit prompt is not Eclipse");
requireMatch("scripts/webos/build_for_lg.sh", /=== Eclipse - Build for LG webOS ===/, "webOS build output still uses the old client name");
requireMatch("scripts/webos/build_for_lg.ps1", /=== Eclipse - Build for LG webOS ===/, "PowerShell build output still uses the old client name");
requireMatch("scripts/tv/lgtv.py", /"localizedAppNames": \{"": "Eclipse TV control"\}/, "TV control registration still uses the old client name");
requireMatch("scripts/bootstrap_client_deps.sh", /Eclipse client dependencies are ready\./, "dependency bootstrap still uses the old client name");
requireMatch("fork/aurora_link/main.cpp", /Eclipse Link — Umbra host companion/, "legacy bundle CLI still presents Aurora or Apollo as the current applications");
requireMatch(
  "fork/aurora_link/mic.cpp",
  /\{L"Eclipse Mic Bridge", L"Aurora Mic Bridge", L"CABLE Input"\}/,
  "legacy bundle microphone lookup does not prefer Eclipse while retaining migration fallbacks",
);
requireMatch("CLAUDE.md", /^# Eclipse\/Umbra — project brief$/m, "active project brief still uses the old identity");
requireMatch("docs/coop-modes.md", /^Eclipse streams in exactly \*\*three\*\* states\./m, "active stream-mode guide still presents the client as Aurora");
requireMatch("src/app/stream/video/coop_trace.c", /# eclipse co-op per-picture trace/, "co-op trace header still presents the client as Aurora");
requireMatch("docs/conducting-live-tests.md", /asserts Umbra is alive on both hosts/, "live-test guide still presents Apollo as the installed host");
requireMatch("scripts/coop/guarded_run.sh", /PRE: Umbra missing on a host/, "guarded-run output still presents Apollo as the installed host");
requireMatch("scripts/coop/capture_host_stream.sh", /Umbra after restart: http=/, "capture output still presents Apollo as the installed host");
requireMatch("scripts/coop/stress_coop.sh", /Umbra up on /, "stress-run output still presents Apollo as the installed host");
requireMatch("scripts/coop/stress_night_run.sh", /peer Umbra up after/, "night-run output still presents Apollo as the installed host");
requireMatch("scripts/coop/teardown.sh", /TV power, Umbra config\./, "teardown guidance still presents Apollo as the installed host");
requireMatch("xlink/host/crash-correlate.sh", /\[UMBRA\]/, "host timeline still labels current events as Apollo");
requireMatch("xlink/host/setup-pc2-controllers.cmd", /Umbra session hook/, "controller setup still presents Apollo as the installed host");
requireMatch("xlink/host/sweep-monitors.sh", /Umbra session events since/, "monitor sweep still presents Apollo as the installed host");
requireMatch("xlink/dispwake/apollo-display.cmd", /layout left as Umbra made it/, "display-hook output still presents Apollo as the installed host");

requireMatch(
  "docs/windows-coop-setup.md",
  /^# Windows Eclipse client and Umbra co-op host setup$/m,
  "consumer setup guide does not identify the applications as Eclipse and Umbra",
);
for (const obsoletePhrase of [
  "Apollo/Umbra host",
  "In Apollo's web interface",
  "allow Apollo through Windows",
  "make sure Apollo is running",
  "Apollo's virtual-display driver",
]) {
  rejectMatch(
    "docs/windows-coop-setup.md",
    new RegExp(obsoletePhrase.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")),
    `setup guide still presents Apollo as the installed host: ${obsoletePhrase}`,
  );
}
requireMatch("docs/BUILD_WEBOS.md", /public application name is \*\*Eclipse\*\*/, "webOS build guide still treats the public name as unresolved");

const i18nRoot = path.join(repositoryRoot, "src", "i18n");
for (const localeEntry of fs.readdirSync(i18nRoot, { withFileTypes: true })) {
  if (localeEntry.isFile() && localeEntry.name.endsWith(".pot")) {
    const relativePath = path.join("src", "i18n", localeEntry.name);
    rejectMatch(relativePath, /\bAurora\b/, "old application branding remains in the message catalogue");
  }
  if (!localeEntry.isDirectory()) continue;
  const relativePath = path.join("src", "i18n", localeEntry.name, "messages.po");
  if (fs.existsSync(path.join(repositoryRoot, relativePath))) {
    rejectMatch(relativePath, /\bAurora\b/, "old application branding remains in a translation catalogue");
  }
}

if (errors.length > 0) {
  console.error("Client-identity check failed:");
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}

console.log("Client-identity check passed: Eclipse replaces Aurora on checked surfaces; inherited Moonlight naming remains.");
