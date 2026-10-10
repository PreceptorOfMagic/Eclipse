#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

let sharp;
try {
  sharp = require("sharp");
} catch {
  console.error(
    "Social-card render failed: the pinned rendering environment needs sharp. " +
    "Install sharp in the active Node environment or expose it through NODE_PATH.",
  );
  process.exit(1);
}

const repositoryRoot = path.resolve(__dirname, "..", "..");
const source = path.join(repositoryRoot, "site", "assets", "og-card.svg");
const destination = path.join(repositoryRoot, "site", "assets", "og-card.png");
const manifest = path.join(repositoryRoot, "site", "assets", "og-card.render.json");
const temporary = `${destination}.tmp`;

function sha256(file) {
  return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

async function main() {
  if (!fs.existsSync(source)) throw new Error(`missing SVG source: ${source}`);

  await sharp(source, { density: 144 })
    .resize(1200, 630, { fit: "fill" })
    .png({ compressionLevel: 9, palette: false })
    .toFile(temporary);

  const metadata = await sharp(temporary).metadata();
  if (metadata.width !== 1200 || metadata.height !== 630 || metadata.format !== "png") {
    throw new Error(`unexpected output: ${metadata.width}x${metadata.height} ${metadata.format}`);
  }

  fs.renameSync(temporary, destination);
  fs.writeFileSync(
    manifest,
    `${JSON.stringify({
      source: "site/assets/og-card.svg",
      sourceSha256: sha256(source),
      output: "site/assets/og-card.png",
      outputSha256: sha256(destination),
      width: 1200,
      height: 630,
      renderer: `sharp ${sharp.versions.sharp}`,
    }, null, 2)}\n`,
  );
  console.log(`Rendered ${path.relative(repositoryRoot, destination)} from ${path.relative(repositoryRoot, source)} (1200x630).`);
}

main().catch((error) => {
  if (fs.existsSync(temporary)) fs.rmSync(temporary);
  console.error(`Social-card render failed: ${error.message}`);
  process.exit(1);
});
