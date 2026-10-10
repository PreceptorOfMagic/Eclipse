#!/usr/bin/env node

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { validateFundingPolicy } from "./funding_policy.mjs";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..", "..");
const result = validateFundingPolicy({ repositoryRoot });

if (result.errors.length > 0) {
  console.error("Funding policy check failed:");
  for (const error of result.errors) console.error(`- ${error}`);
  process.exit(1);
}

if (result.mode === "enabled") {
  console.log(`Funding policy check passed: Buy Me a Coffee recipient ${result.username} matches the disclosed payment route.`);
} else {
  console.log("Funding policy check passed: funding is consistently disabled.");
}
