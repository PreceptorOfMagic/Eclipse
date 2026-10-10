#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..", "..");
const siteRoot = path.join(repositoryRoot, "site");
const errors = [];

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const target = path.join(directory, entry.name);
    if (entry.isSymbolicLink()) {
      errors.push(`${path.relative(repositoryRoot, target)}: symbolic links are not allowed in the public site`);
      return [];
    }
    return entry.isDirectory() ? walk(target) : [target];
  });
}

if (!fs.existsSync(siteRoot)) {
  console.error("Public-site check failed: site/ does not exist.");
  process.exit(1);
}

const allFiles = walk(siteRoot);
const htmlFiles = allFiles.filter((file) => file.endsWith(".html"));
const requiredPages = [
  "index.html",
  "projects.html",
  "setup.html",
  "media.html",
  "credits.html",
  "support.html",
  "walkthrough.html",
  "404.html",
];
const requiredNavigation = [
  ["Co-op", "./#coop"],
  ["Applications", "./#applications"],
  ["Projects", "projects.html"],
  ["Setup", "setup.html"],
  ["Media", "media.html"],
  ["Status", "./#status"],
  ["Credits", "credits.html"],
  ["Support", "support.html"],
];
const requiredFooterLinks = [
  ["ECLIPSE / UMBRA", "./"],
  ["Eclipse client source", "https://github.com/PreceptorOfMagic/eclipse"],
  ["Umbra host source", "https://github.com/PreceptorOfMagic/umbra"],
  ["Eclipse issues", "https://github.com/PreceptorOfMagic/eclipse/issues"],
  ["Umbra issues", "https://github.com/PreceptorOfMagic/umbra/issues"],
  ["Acknowledgements", "credits.html"],
  ["Project map", "projects.html"],
  ["Media plan", "media.html"],
  ["Support", "support.html"],
  ["Release status", "./#status"],
  ["Contributing", "https://github.com/PreceptorOfMagic/eclipse/blob/main/CONTRIBUTING.md"],
  ["Repository licence text", "https://github.com/PreceptorOfMagic/eclipse/blob/main/LICENSE.txt"],
];
const canonicalUrls = new Map([
  ["index.html", "https://preceptorofmagic.github.io/eclipse/"],
  ["projects.html", "https://preceptorofmagic.github.io/eclipse/projects.html"],
  ["setup.html", "https://preceptorofmagic.github.io/eclipse/setup.html"],
  ["media.html", "https://preceptorofmagic.github.io/eclipse/media.html"],
  ["credits.html", "https://preceptorofmagic.github.io/eclipse/credits.html"],
  ["support.html", "https://preceptorofmagic.github.io/eclipse/support.html"],
  ["walkthrough.html", "https://preceptorofmagic.github.io/eclipse/walkthrough.html"],
]);

function extractAnchors(fragment) {
  return [...fragment.matchAll(/<a\b([^>]*)>([\s\S]*?)<\/a>/gi)].map((match) => {
    const href = match[1].match(/\bhref=["']([^"']+)["']/i)?.[1] ?? "";
    const label = match[2].replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim();
    return [label, href];
  });
}

function compareLinkContract(relative, contractName, actual, expected) {
  if (actual.length !== expected.length) {
    errors.push(`${relative}: ${contractName} has ${actual.length} links; expected ${expected.length}`);
    return;
  }
  for (let index = 0; index < expected.length; index += 1) {
    if (actual[index][0] !== expected[index][0] || actual[index][1] !== expected[index][1]) {
      errors.push(
        `${relative}: ${contractName} link ${index + 1} is ` +
        `"${actual[index][0]}" (${actual[index][1]}); expected ` +
        `"${expected[index][0]}" (${expected[index][1]})`,
      );
    }
  }
}

for (const page of requiredPages) {
  if (!fs.existsSync(path.join(siteRoot, page))) {
    errors.push(`site/${page}: required page is missing`);
  }
}

const notFoundPage = path.join(siteRoot, "404.html");
if (fs.existsSync(notFoundPage)) {
  const notFoundHtml = fs.readFileSync(notFoundPage, "utf8");
  if (!/<base\b[^>]*href=["']https:\/\/preceptorofmagic\.github\.io\/eclipse\/["'][^>]*>/i.test(notFoundHtml)) {
    errors.push("site/404.html: missing canonical project base; nested missing URLs would break relative assets and navigation");
  }
}

const htmlByFile = new Map();
const idsByFile = new Map();

for (const file of htmlFiles) {
  const html = fs.readFileSync(file, "utf8");
  htmlByFile.set(file, html);

  const relative = path.relative(repositoryRoot, file);
  const ids = [...html.matchAll(/\bid=["']([^"']+)["']/gi)].map((match) => match[1]);
  const uniqueIds = new Set();
  for (const id of ids) {
    if (uniqueIds.has(id)) errors.push(`${relative}: duplicate id "${id}"`);
    uniqueIds.add(id);
  }
  idsByFile.set(file, uniqueIds);

  if (!/^<!doctype html>/i.test(html.trimStart())) errors.push(`${relative}: missing HTML doctype`);
  if (!/<html\b[^>]*\blang=["'][^"']+["']/i.test(html)) errors.push(`${relative}: missing html language`);
  if (!/<meta\b[^>]*\bname=["']viewport["']/i.test(html)) errors.push(`${relative}: missing viewport metadata`);
  if (!/<title>[^<]+<\/title>/i.test(html)) errors.push(`${relative}: missing non-empty title`);
  if (path.basename(file) !== "404.html") {
    if (!/<meta\b[^>]*\bname=["']description["'][^>]*\bcontent=["'][^"']+["']/i.test(html)) {
      errors.push(`${relative}: missing non-empty description metadata`);
    }
    for (const property of ["og:title", "og:description", "og:url", "og:image", "og:image:alt"]) {
      const escapedProperty = property.replace(":", "\\:");
      if (!new RegExp(`<meta\\b[^>]*\\bproperty=["']${escapedProperty}["'][^>]*\\bcontent=["'][^"']+["']`, "i").test(html)) {
        errors.push(`${relative}: missing ${property} metadata`);
      }
    }
    if (!/<meta\b[^>]*\bname=["']twitter:card["'][^>]*\bcontent=["']summary_large_image["']/i.test(html)) {
      errors.push(`${relative}: missing large-image Twitter card metadata`);
    }
    for (const name of ["twitter:image", "twitter:image:alt"]) {
      const escapedName = name.replace(":", "\\:");
      if (!new RegExp(`<meta\\b[^>]*\\bname=["']${escapedName}["'][^>]*\\bcontent=["'][^"']+["']`, "i").test(html)) {
        errors.push(`${relative}: missing ${name} metadata`);
      }
    }
    const expectedCanonical = canonicalUrls.get(path.basename(file));
    if (!expectedCanonical) {
      errors.push(`${relative}: no canonical URL is registered in the site check`);
    } else {
      const canonical = html.match(/<link\b[^>]*\brel=["']canonical["'][^>]*\bhref=["']([^"']+)["']/i)?.[1];
      const openGraphUrl = html.match(/<meta\b[^>]*\bproperty=["']og:url["'][^>]*\bcontent=["']([^"']+)["']/i)?.[1];
      if (canonical !== expectedCanonical) errors.push(`${relative}: canonical URL must be ${expectedCanonical}`);
      if (openGraphUrl !== expectedCanonical) errors.push(`${relative}: og:url must be ${expectedCanonical}`);
    }
    const expectedSocialImage = "https://preceptorofmagic.github.io/eclipse/assets/og-card.png";
    const openGraphImage = html.match(/<meta\b[^>]*\bproperty=["']og:image["'][^>]*\bcontent=["']([^"']+)["']/i)?.[1];
    const twitterImage = html.match(/<meta\b[^>]*\bname=["']twitter:image["'][^>]*\bcontent=["']([^"']+)["']/i)?.[1];
    if (openGraphImage !== expectedSocialImage) errors.push(`${relative}: og:image must use the canonical social card URL`);
    if (twitterImage !== expectedSocialImage) errors.push(`${relative}: twitter:image must use the canonical social card URL`);
  } else if (!/<meta\b[^>]*\bname=["']robots["'][^>]*\bcontent=["'][^"']*noindex/i.test(html)) {
    errors.push(`${relative}: 404 page must be noindex`);
  }
  if (!/<main\b/i.test(html)) errors.push(`${relative}: missing main landmark`);
  if (!/<h1\b[^>]*>[^<]+/i.test(html)) errors.push(`${relative}: missing a visible h1`);
  const expectedSkipTarget = path.basename(file) === "404.html" ? "404.html#main" : "#main";
  const skipTarget = html.match(/<a\b[^>]*class=["'][^"']*skip-link[^"']*["'][^>]*href=["']([^"']+)["']/i)?.[1];
  if (skipTarget !== expectedSkipTarget) {
    errors.push(`${relative}: skip link must target ${expectedSkipTarget}`);
  }
  if (!/<header\b[^>]*class=["'][^"']*site-header/i.test(html)) errors.push(`${relative}: missing shared site header`);
  if (!/<footer\b[^>]*class=["'][^"']*site-footer/i.test(html)) errors.push(`${relative}: missing shared site footer`);

  const navigation = html.match(/<nav\b[^>]*aria-label=["']Primary navigation["'][^>]*>([\s\S]*?)<\/nav>/i)?.[1] ?? "";
  if (!navigation) {
    errors.push(`${relative}: missing labelled primary navigation`);
  } else {
    compareLinkContract(relative, "primary navigation", extractAnchors(navigation), requiredNavigation);
  }

  const footer = html.match(/<footer\b[^>]*class=["'][^"']*site-footer[^"']*["'][^>]*>([\s\S]*?)<\/footer>/i)?.[1] ?? "";
  if (footer) {
    compareLinkContract(relative, "shared footer", extractAnchors(footer), requiredFooterLinks);
    if (!/Eclipse\/Umbra is one project: Eclipse is its client application and Umbra is its host application\./i.test(footer)) {
      errors.push(`${relative}: shared footer is missing the project/application hierarchy`);
    }
  }

  for (const match of html.matchAll(/<img\b[^>]*>/gi)) {
    if (!/\balt=["'][^"']*["']/i.test(match[0])) {
      errors.push(`${relative}: image is missing an alt attribute: ${match[0]}`);
    }
  }

  for (const match of html.matchAll(/<video\b[^>]*>/gi)) {
    if (/\bautoplay\b/i.test(match[0]) && !/\bcontrols\b/i.test(match[0])) {
      errors.push(`${relative}: autoplaying video has no visible playback controls`);
    }
  }

  for (const match of html.matchAll(/<table\b[^>]*>([\s\S]*?)<\/table>/gi)) {
    if (!/<caption\b/i.test(match[1])) errors.push(`${relative}: table is missing a caption`);
    for (const header of match[1].matchAll(/<th\b[^>]*>/gi)) {
      if (!/\bscope=["'](?:col|row|colgroup|rowgroup)["']/i.test(header[0])) {
        errors.push(`${relative}: table header is missing scope: ${header[0]}`);
      }
    }
  }

  const forbiddenPatterns = [
    [/(?:^|["'(/])scratchpad\//i, "references diagnostic scratchpad content"],
    [/\blocalhost\b|127\.0\.0\.1/i, "contains a local-only address"],
    [/\[(?:DIRECT DEMO URL|CANONICAL HUB URL|CLIENT NAME|HOST NAME|STATUS URL)\]/, "contains an unfilled launch placeholder"],
  ];
  for (const [pattern, message] of forbiddenPatterns) {
    if (pattern.test(html)) errors.push(`${relative}: ${message}`);
  }
}

const stylesheet = fs.readFileSync(path.join(siteRoot, "assets", "styles.css"), "utf8");
if (/\.main-nav\s+a:not\(\.button\)[^{]*\{[^}]*display\s*:\s*none/i.test(stylesheet)) {
  errors.push("site/assets/styles.css: responsive rules hide ordinary primary-navigation links");
}

let localReferenceCount = 0;

for (const [file, html] of htmlByFile) {
  const relative = path.relative(repositoryRoot, file);
  for (const match of html.matchAll(/\b(?:href|src)=["']([^"']+)["']/gi)) {
    const reference = match[1];
    if (/^(?:https?:|mailto:)/i.test(reference)) continue;
    if (reference.startsWith("//")) continue;
    if (reference.startsWith("/")) {
      errors.push(`${relative}: root-relative reference breaks on a project Pages path: ${reference}`);
      continue;
    }

    localReferenceCount += 1;
    const [beforeFragment, fragment] = reference.split("#", 2);
    const pathPart = beforeFragment.split("?", 1)[0];
    let target = pathPart ? path.resolve(path.dirname(file), decodeURIComponent(pathPart)) : file;
    if (fs.existsSync(target) && fs.statSync(target).isDirectory()) target = path.join(target, "index.html");

    if (target !== siteRoot && !target.startsWith(`${siteRoot}${path.sep}`)) {
      errors.push(`${relative}: reference escapes site/: ${reference}`);
      continue;
    }
    if (!fs.existsSync(target)) {
      errors.push(`${relative}: missing local target: ${reference}`);
      continue;
    }
    if (fragment) {
      if (!target.endsWith(".html")) {
        errors.push(`${relative}: fragment targets a non-HTML file: ${reference}`);
        continue;
      }
      const targetIds = idsByFile.get(target);
      if (!targetIds?.has(decodeURIComponent(fragment))) {
        errors.push(`${relative}: missing fragment target: ${reference}`);
      }
    }
  }
}

if (errors.length > 0) {
  console.error("Public-site check failed:");
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}

console.log(`Public-site check passed: ${htmlFiles.length} HTML pages and ${localReferenceCount} local references.`);
