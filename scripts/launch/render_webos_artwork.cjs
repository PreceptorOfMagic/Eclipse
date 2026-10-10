#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

let sharp;
try {
  sharp = require("sharp");
} catch {
  console.error(
    "webOS artwork render failed: install the launch tools with " +
    "`npm ci --prefix scripts/launch` first.",
  );
  process.exit(1);
}

const repositoryRoot = path.resolve(__dirname, "..", "..");
const sources = [
  "site/assets/brand-mark.svg",
  "scripts/launch/assets/webos-splash.svg",
  "scripts/launch/assets/webos-icon.svg",
];
const outputs = [
  { source: sources[2], output: "deploy/webos/icon.png", width: 80, height: 80 },
  { source: sources[2], output: "deploy/webos/icon_large.png", width: 130, height: 130 },
  { source: sources[1], output: "deploy/webos/splash.png", width: 1920, height: 1080 },
];
const manifestPath = path.join(__dirname, "assets", "webos-artwork.render.json");

function absolute(relativePath) {
  return path.join(repositoryRoot, relativePath);
}

function sha256(file) {
  return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

async function main() {
  for (const source of sources) {
    if (!fs.existsSync(absolute(source))) throw new Error(`missing SVG source: ${source}`);
  }

  for (const item of outputs) {
    const destination = absolute(item.output);
    const temporary = `${destination}.tmp`;
    try {
      await sharp(absolute(item.source), { density: 384 })
        .resize(item.width, item.height, { fit: "fill" })
        .png({ compressionLevel: 9, palette: false })
        .toFile(temporary);
      const metadata = await sharp(temporary).metadata();
      if (metadata.width !== item.width || metadata.height !== item.height || metadata.format !== "png") {
        throw new Error(`${item.output}: unexpected ${metadata.width}x${metadata.height} ${metadata.format}`);
      }
      fs.renameSync(temporary, destination);
    } catch (error) {
      if (fs.existsSync(temporary)) fs.rmSync(temporary);
      throw error;
    }
  }

  const manifest = {
    renderer: `sharp ${sharp.versions.sharp}`,
    sources: sources.map((source) => ({ path: source, sha256: sha256(absolute(source)) })),
    outputs: outputs.map((item) => ({
      path: item.output,
      source: item.source,
      sha256: sha256(absolute(item.output)),
      width: item.width,
      height: item.height,
    })),
  };
  fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
  console.log("Rendered Eclipse/Umbra webOS icon, large icon and splash artwork.");
}

main().catch((error) => {
  console.error(`webOS artwork render failed: ${error.message}`);
  process.exit(1);
});
