import fs from "node:fs";
import path from "node:path";

const paymentLabels = [
  "Legal recipient",
  "Purpose",
  "Fees",
  "Currency",
  "Recurring",
  "Cancellation",
  "Refund",
  "Privacy",
  "Tax",
  "Contact",
  "Closure",
];

export function validateFundingPolicy({ repositoryRoot, siteRoot = path.join(repositoryRoot, "site") }) {
  const errors = [];
  const fundingConfiguration = fs.readFileSync(
    path.join(repositoryRoot, ".github", "FUNDING.yml"),
    "utf8",
  );
  const activeEntries = fundingConfiguration
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0 && !line.startsWith("#"));
  const supportPage = fs.readFileSync(path.join(siteRoot, "support.html"), "utf8");
  const readme = fs.readFileSync(path.join(repositoryRoot, "README.md"), "utf8");
  const supportPolicy = fs.readFileSync(path.join(repositoryRoot, "SUPPORT.md"), "utf8");
  const siteHtmlFiles = fs.readdirSync(siteRoot)
    .filter((name) => name.endsWith(".html"))
    .map((name) => ({ name, text: fs.readFileSync(path.join(siteRoot, name), "utf8") }));

  for (const { name, text } of siteHtmlFiles) {
    if (/cdnjs\.buymeacoffee\.com|button\.prod\.min\.js/i.test(text)) {
      errors.push(`site/${name}: load Buy Me a Coffee through a semantic link, not its third-party button script`);
    }
    if (name !== "support.html" && /href=["']https:\/\/(?:www\.)?buymeacoffee\.com\//i.test(text)) {
      errors.push(`site/${name}: direct payment links must route through site/support.html and its disclosures`);
    }
  }

  if (activeEntries.length === 0) {
    if (!/Funding is not enabled\./i.test(supportPage)) {
      errors.push("site/support.html: disabled funding status is missing while FUNDING.yml has no recipient");
    }
    if (!/<button\b[^>]*\bdisabled\b[^>]*>\s*Buy Me a Coffee link coming soon\s*<\/button>/i.test(supportPage)) {
      errors.push("site/support.html: native disabled Buy Me a Coffee button is missing while funding remains disabled");
    }
    return { errors, mode: "disabled", username: null };
  }

  let username = null;
  if (activeEntries.length !== 1) {
    errors.push(".github/FUNDING.yml: Buy Me a Coffee must be the sole active funding entry");
  } else {
    const fundingMatch = activeEntries[0].match(
      /^buy_me_a_coffee:\s*(["']?)([A-Za-z0-9][A-Za-z0-9._-]*)\1$/,
    );
    if (!fundingMatch || /^(?:verified[_-]?bmc[_-]?username|username|placeholder)$/i.test(fundingMatch?.[2] ?? "")) {
      errors.push(".github/FUNDING.yml: expected buy_me_a_coffee: VERIFIED_USERNAME with a real username");
    } else {
      username = fundingMatch[2];
    }
  }

  if (/Funding is not enabled\.|Buy Me a Coffee link coming soon/i.test(supportPage)) {
    errors.push("site/support.html: funding configuration and public support status disagree");
  }

  const paymentAnchor = (supportPage.match(/<a\b[^>]*>/gi) ?? []).find((tag) =>
    /\bclass=["'][^"']*\bsupport-payment\b[^"']*["']/i.test(tag),
  );
  const paymentHref = paymentAnchor?.match(/\bhref=["']([^"']+)["']/i)?.[1] ?? "";
  if (!paymentHref) {
    errors.push("site/support.html: enabled funding requires a real HTTPS .support-payment link");
  } else if (username) {
    try {
      const paymentUrl = new URL(paymentHref);
      const paymentHost = paymentUrl.hostname.toLowerCase().replace(/^www\./, "");
      const paymentUsername = decodeURIComponent(paymentUrl.pathname).replace(/^\/+|\/+$/g, "");
      if (
        paymentUrl.protocol !== "https:"
        || paymentHost !== "buymeacoffee.com"
        || paymentUsername.toLowerCase() !== username.toLowerCase()
        || paymentUrl.search
        || paymentUrl.hash
      ) {
        errors.push("site/support.html: .support-payment must match the configured Buy Me a Coffee username");
      }
    } catch {
      errors.push("site/support.html: .support-payment is not a valid Buy Me a Coffee HTTPS URL");
    }
  }

  if (paymentAnchor && !/\btarget=["']_blank["']/i.test(paymentAnchor)) {
    errors.push("site/support.html: external payment link must declare its new-tab behaviour");
  }
  const rel = paymentAnchor?.match(/\brel=["']([^"']+)["']/i)?.[1]?.toLowerCase() ?? "";
  if (paymentAnchor && (!rel.split(/\s+/).includes("noopener") || !rel.split(/\s+/).includes("noreferrer"))) {
    errors.push("site/support.html: external payment link must use rel=noopener noreferrer");
  }
  if (!/opens in a new tab/i.test(supportPage)) {
    errors.push("site/support.html: external payment link needs a screen-reader new-tab notice");
  }

  const disclosure = supportPage.match(
    /<(?:section|div)\b[^>]*class=["'][^"']*support-disclosure[^"']*["'][^>]*>([\s\S]*?)<\/(?:section|div)>/i,
  )?.[1] ?? "";
  if (!disclosure) {
    errors.push("site/support.html: enabled funding requires a visible .support-disclosure block beside the payment action");
  } else {
    for (const label of paymentLabels) {
      if (!new RegExp(`\\b${label}\\b`, "i").test(disclosure)) {
        errors.push(`site/support.html: enabled funding disclosure is missing ${label}`);
      }
    }
    for (const [pattern, message] of [
      [/one-time or monthly/i, "recurring choice"],
      [/outside Australia/i, "overseas data processing"],
      [/not (?:an? )?(?:Australian )?tax-deductible/i, "non-deductibility"],
    ]) {
      if (!pattern.test(disclosure)) {
        errors.push(`site/support.html: enabled funding disclosure is missing ${message}`);
      }
    }
  }

  if (username) {
    const canonical = `https://buymeacoffee.com/${username}`.toLowerCase();
    if (!readme.toLowerCase().includes(canonical)) {
      errors.push("README.md: Buy Me a Coffee destination does not match FUNDING.yml");
    }
    if (!supportPolicy.toLowerCase().includes(canonical)) {
      errors.push("SUPPORT.md: Buy Me a Coffee destination does not match FUNDING.yml");
    }
  }

  return { errors, mode: "enabled", username };
}
