#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import crypto from "node:crypto";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { validateFundingPolicy } from "./funding_policy.mjs";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..", "..");
const siteRoot = path.join(repositoryRoot, "site");
const errors = [];

const publicationChecklist = fs.readFileSync(
  path.join(repositoryRoot, "docs", "launch", "publication-checklist.md"),
  "utf8",
);
const fundingGateHeading = "## 7. Live support configuration and follow-up";
const fundingGateOffset = publicationChecklist.indexOf(fundingGateHeading);
if (fundingGateOffset === -1) {
  errors.push("docs/launch/publication-checklist.md: live support section is missing");
}
const launchChecklist = fundingGateOffset === -1
  ? publicationChecklist
  : publicationChecklist.slice(0, fundingGateOffset);
const openChecklistItems = launchChecklist.match(/^- \[ \] /gm)?.length ?? 0;
if (openChecklistItems > 0) {
  errors.push(`docs/launch/publication-checklist.md: ${openChecklistItems} release attestations remain open`);
}

errors.push(...validateFundingPolicy({ repositoryRoot, siteRoot }).errors);

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(file) : [file];
  });
}

const publicTextExtensions = new Set([".css", ".html", ".md", ".svg", ".txt", ".vtt", ".xml"]);
const publicTextFiles = [
  path.join(repositoryRoot, "README.md"),
  path.join(repositoryRoot, "ACKNOWLEDGEMENTS.md"),
  path.join(repositoryRoot, "SUPPORT.md"),
  ...walk(siteRoot),
]
  .filter((file) => publicTextExtensions.has(path.extname(file).toLowerCase()));
const publicText = publicTextFiles
  .map((file) => `${path.relative(repositoryRoot, file)}\n${fs.readFileSync(file, "utf8")}`)
  .join("\n");
const communityPosts = fs.readFileSync(
  path.join(repositoryRoot, "docs", "launch", "community-posts.md"),
  "utf8",
);
const unresolvedCommunityTokens = communityPosts.match(
  /\[(?:DEMO URL|HUB URL|SETUP URL|STATUS URL|ACKNOWLEDGEMENTS URL|ECLIPSE SOURCE URL|UMBRA SOURCE URL)\]/g,
) ?? [];
if (unresolvedCommunityTokens.length > 0) {
  errors.push(`docs/launch/community-posts.md: ${unresolvedCommunityTokens.length} launch-link placeholders remain`);
}

const blockers = [
  [/ECLIPSE\s*[×x]\s*UMBRA/i, "legacy Eclipse × Umbra lock-up remains; use the canonical Eclipse/Umbra project identity"],
  [/public source (?:is )?in preparation/i, "prelaunch source wording remains"],
  [/(?:repositories|installers?) (?:are |is )?not public/i, "private/prelaunch repository or installer wording remains"],
  [/no public package|no consumer installers? (?:are |is )?public|public packages? (?:are |is )?not available/i, "no-public-package wording remains"],
  [/engineering preview/i, "engineering-preview wording remains"],
  [/recording pending|hero video · planned|walkthrough · planned|photography · planned/i, "planned-media wording remains"],
  [/walkthrough has not been recorded or published|walkthrough pending recording/i, "walkthrough/transcript page is still in its honest pre-recording state"],
  [/launch blockers? rather than completed|remain launch gates?|public release still needs|no generated release artifact has yet|no current generated .* package has (?:yet )?been approved or released/i,
    "release-blocker wording remains in public acknowledgements or site copy"],
  [/there is no (?:reviewed )?public installer yet|public links? and releases? remain gated|release-compliance work still open|no public release may|release artifacts? will be published/i,
    "additional prelaunch/release-blocker wording remains"],
  [/public release artifacts? have not yet been frozen|unpublished development source|no final installer or release artifact has been verified|(?:release-branch integration|integration into the release branch) is unfinished/i,
    "unfinished release-status wording remains"],
  [/no (?:newly )?generated package has yet been inspected|remaining client work includes|generated host artifacts[^.]*remain unverified|installer[^.]*still needs its artifact-specific/i,
    "unverified package/compliance wording remains"],
];
for (const [pattern, message] of blockers) {
  if (pattern.test(publicText)) errors.push(message);
}

const unresolvedMediaPages = walk(siteRoot)
  .filter((file) => file.endsWith(".html"))
  .filter((file) => /class=["'][^"']*(?:showcase-placeholder|media-mini-frame)[^"']*["']/i.test(fs.readFileSync(file, "utf8")))
  .map((file) => path.relative(repositoryRoot, file));
if (unresolvedMediaPages.length > 0) {
  errors.push(`planned-media placeholder elements remain in ${unresolvedMediaPages.join(", ")}`);
}

const socialCard = path.join(siteRoot, "assets", "og-card.png");
const socialCardSource = path.join(siteRoot, "assets", "og-card.svg");
const socialCardManifest = path.join(siteRoot, "assets", "og-card.render.json");
const prelaunchSocialCardHash = "310448697454cbea5f21c79033cdb838647280b5063dbdaa74b9aededbb523ee";
if (!fs.existsSync(socialCard)) {
  errors.push("site/assets/og-card.png: social card is missing");
} else {
  const socialCardBytes = fs.readFileSync(socialCard);
  if (socialCardBytes.length < 24 || socialCardBytes.toString("hex", 0, 8) !== "89504e470d0a1a0a") {
    errors.push("site/assets/og-card.png: social card is not a valid PNG");
  } else {
    const width = socialCardBytes.readUInt32BE(16);
    const height = socialCardBytes.readUInt32BE(20);
    if (width !== 1200 || height !== 630) {
      errors.push(`site/assets/og-card.png: expected 1200x630, found ${width}x${height}`);
    }
  }
  const socialCardHash = crypto.createHash("sha256").update(fs.readFileSync(socialCard)).digest("hex");
  if (socialCardHash === prelaunchSocialCardHash) {
    errors.push("site/assets/og-card.png: working-name social card has not been regenerated");
  }
}
if (!fs.existsSync(socialCardSource)) {
  errors.push("site/assets/og-card.svg: editable social-card source is missing");
}
if (!fs.existsSync(socialCardManifest)) {
  errors.push("site/assets/og-card.render.json: social-card render manifest is missing");
} else if (fs.existsSync(socialCard) && fs.existsSync(socialCardSource)) {
  try {
    const manifest = JSON.parse(fs.readFileSync(socialCardManifest, "utf8"));
    const sourceHash = crypto.createHash("sha256").update(fs.readFileSync(socialCardSource)).digest("hex");
    const outputHash = crypto.createHash("sha256").update(fs.readFileSync(socialCard)).digest("hex");
    if (manifest.sourceSha256 !== sourceHash || manifest.outputSha256 !== outputHash) {
      errors.push("site/assets/og-card.png: render manifest does not match the current SVG source and PNG output");
    }
    if (manifest.width !== 1200 || manifest.height !== 630 || manifest.renderer !== "sharp 0.35.4") {
      errors.push("site/assets/og-card.render.json: dimensions or pinned renderer do not match the launch contract");
    }
  } catch (error) {
    errors.push(`site/assets/og-card.render.json: invalid render manifest (${error.message})`);
  }
}
if (!fs.existsSync(path.join(scriptDirectory, "render_social_card.cjs"))) {
  errors.push("scripts/launch/render_social_card.cjs: deterministic social-card renderer is missing");
}
const launchToolsPackage = path.join(scriptDirectory, "package.json");
const launchToolsLock = path.join(scriptDirectory, "package-lock.json");
if (!fs.existsSync(launchToolsPackage) || !fs.existsSync(launchToolsLock)) {
  errors.push("scripts/launch: package.json/package-lock.json must pin the social-card renderer");
} else {
  try {
    const packageData = JSON.parse(fs.readFileSync(launchToolsPackage, "utf8"));
    const lockData = JSON.parse(fs.readFileSync(launchToolsLock, "utf8"));
    const packageSharp = packageData.dependencies?.sharp;
    const lockedSharp = lockData.packages?.["node_modules/sharp"]?.version;
    if (packageSharp !== "0.35.4" || lockedSharp !== "0.35.4") {
      errors.push("scripts/launch: sharp must be pinned and locked at 0.35.4 for the social-card render contract");
    }
  } catch (error) {
    errors.push(`scripts/launch: invalid renderer package manifest or lock (${error.message})`);
  }
}

const index = fs.readFileSync(path.join(siteRoot, "index.html"), "utf8");
for (const [pattern, message] of [
  [/ECLIPSE \/ UMBRA/, "site/index.html: canonical Eclipse/Umbra project lock-up is missing"],
  [/Eclipse client/i, "site/index.html: Eclipse is not clearly identified as the client application"],
  [/Umbra host/i, "site/index.html: Umbra is not clearly identified as the host application"],
]) {
  if (!pattern.test(index)) errors.push(message);
}
const heroSection = index.match(/<section\b[^>]*class=["'][^"']*\bhero\b[^"']*["'][^>]*>([\s\S]*?)<\/section>/i)?.[1] ?? "";
const heroVideoTag = heroSection.match(/<video\b[^>]*id=["']coop-hero-video["'][^>]*>/i)?.[0] ?? "";
if (!heroVideoTag) {
  errors.push("site/index.html: accepted hero video is not integrated above the fold");
} else {
  for (const attribute of ["muted", "loop", "playsinline", "controls"]) {
    if (!new RegExp(`\\b${attribute}\\b`, "i").test(heroVideoTag)) {
      errors.push(`site/index.html: hero video is missing ${attribute}`);
    }
  }
  if (/\bautoplay\b/i.test(heroVideoTag)) {
    errors.push("site/index.html: hero video must omit the autoplay attribute so reduced-motion users start on the poster");
  }
  if (!/matchMedia\(["']\(prefers-reduced-motion:\s*reduce\)["']\)/i.test(index) ||
      !/heroVideo\.pause\(\)/.test(index) || !/heroVideo\.play\(\)/.test(index)) {
    errors.push("site/index.html: hero playback is not gated by the reduced-motion preference");
  }
}
for (const name of [
  "coop-hero-loop.webm",
  "coop-hero-loop.mp4",
  "coop-hero-poster.webp",
  "living-room-wide.webp",
  "coop-picker.webp",
  "coop-walkthrough.en.vtt",
]) {
  if (!index.includes(`media/${name}`)) errors.push(`site/index.html: media/${name} is not referenced`);
}
if (!index.includes("walkthrough.html")) errors.push("site/index.html: accessible walkthrough page is not linked");
const walkthroughPage = path.join(siteRoot, "walkthrough.html");
if (!fs.existsSync(walkthroughPage)) {
  errors.push("site/walkthrough.html: accessible walkthrough/transcript page is missing");
} else {
  const walkthrough = fs.readFileSync(walkthroughPage, "utf8");
  if (!walkthrough.includes("media/coop-walkthrough.en.vtt")) {
    errors.push("site/walkthrough.html: reviewed caption file is not linked");
  }
  if (!walkthrough.includes("media/coop-walkthrough-transcript.md")) {
    errors.push("site/walkthrough.html: reviewed Markdown transcript source is not linked");
  }
  if (!/<h2[^>]*>\s*Visual description\s*<\/h2>/i.test(walkthrough)) {
    errors.push("site/walkthrough.html: semantic visual-description section is missing");
  }
}

const mediaCheck = spawnSync(process.execPath, [path.join(scriptDirectory, "check_media.mjs")], {
  cwd: repositoryRoot,
  encoding: "utf8",
});
if (mediaCheck.status !== 0) {
  errors.push("launch media has not passed scripts/launch/check_media.mjs");
}

const identityCheck = spawnSync(process.execPath, [path.join(scriptDirectory, "check_client_identity.mjs")], {
  cwd: repositoryRoot,
  encoding: "utf8",
});
if (identityCheck.status !== 0) {
  errors.push("client identity has not passed scripts/launch/check_client_identity.mjs");
}

if (errors.length > 0) {
  console.error("Full-launch quality audit is incomplete:");
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}

console.log("Public-launch gate passed: release checklist, public copy and required media are ready.");
