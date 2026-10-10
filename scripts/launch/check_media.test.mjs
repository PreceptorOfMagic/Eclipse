import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  binaryPrintableSafetyProblems,
  compareFrameTimelines,
  hasInvalidRotationMetadata,
  hasUnexpectedChapters,
  inventoryMediaDirectory,
  isPublicHttpsUrl,
  inspectWebP,
  parseMp4TopLevelAtoms,
  probeProcessFailed,
  parseSsim,
  parseSsimFrameStats,
  parseWebVtt,
  validateMp4FastStart,
  validateManifest,
  validateTranscript,
  validateTechnicalMetadata,
  validateVideoMeasurements,
  unexpectedNonVideoStreamTypes,
} from "./check_media.mjs";

function mp4Atom(type, payload = Buffer.alloc(0)) {
  const atom = Buffer.alloc(8 + payload.length);
  atom.writeUInt32BE(atom.length, 0);
  atom.write(type, 4, 4, "ascii");
  payload.copy(atom, 8);
  return atom;
}

function webpChunk(type, payload = Buffer.alloc(0)) {
  const chunk = Buffer.alloc(8 + payload.length + (payload.length % 2));
  chunk.write(type, 0, 4, "ascii");
  chunk.writeUInt32LE(payload.length, 4);
  payload.copy(chunk, 8);
  return chunk;
}

function webpFile(...chunks) {
  const body = Buffer.concat([Buffer.from("WEBP", "ascii"), ...chunks]);
  const header = Buffer.alloc(8);
  header.write("RIFF", 0, 4, "ascii");
  header.writeUInt32LE(body.length, 4);
  return Buffer.concat([header, body]);
}

function deliveryReview(sha256, lineage) {
  return {
    sha256,
    lineage: [lineage],
    rightsReviewed: true,
    privacyReviewed: true,
    accessibilityReviewed: true,
    reviewedBy: "release-reviewer",
    reviewedAt: "2026-10-01",
  };
}

function sampleManifest() {
  const hashes = new Map([
    ["coop-hero-loop.webm", "a".repeat(64)],
    ["coop-hero-loop.mp4", "b".repeat(64)],
    ["coop-hero-poster.webp", "c".repeat(64)],
    ["living-room-wide.webp", "d".repeat(64)],
    ["coop-picker.webp", "e".repeat(64)],
    ["coop-walkthrough.en.vtt", "f".repeat(64)],
    ["coop-walkthrough-transcript.md", "0".repeat(64)],
  ]);
  const sourceReview = {
    publicSafe: true,
    rightsReviewId: "rights-review-01",
    privacyReviewId: "privacy-review-01",
    consentReviewId: "consent-review-01",
    reviewedBy: "release-reviewer",
    reviewedAt: "2026-10-01",
  };
  const manifest = {
    schemaVersion: 1,
    walkthrough: {
      url: "https://www.youtube.com/watch?v=AbCdEfGhI12",
      durationSeconds: 100,
      sourceId: "walkthrough-master-01",
      signedOutPlaybackReviewed: true,
      captionsEnabledReviewed: true,
      reviewedBy: "release-reviewer",
      reviewedAt: "2026-10-01",
    },
    builds: {
      eclipse: { commit: "1".repeat(40), version: "1.2.3.4", artifactSha256: "2".repeat(64) },
      umbraHostA: { commit: "3".repeat(40), version: "1.0.0", artifactSha256: "4".repeat(64) },
      umbraHostB: { commit: "5".repeat(40), version: "1.0.0", artifactSha256: "6".repeat(64) },
    },
    sourceInventory: [
      {
        id: "proof-take-01",
        kind: "continuous-live-take",
        publicLabel: "Continuous live co-op proof take",
        sha256: "7".repeat(64),
        durationSeconds: 30,
        frameRate: 30,
        frameCount: 900,
        liveCapture: true,
        ...sourceReview,
      },
      {
        id: "room-photo-01",
        kind: "photograph",
        publicLabel: "Reviewed shared-room photograph",
        sha256: "8".repeat(64),
        liveCapture: false,
        ...sourceReview,
      },
      {
        id: "picker-still-01",
        kind: "screenshot",
        publicLabel: "Reviewed co-op picker screenshot",
        sha256: "9".repeat(64),
        liveCapture: true,
        ...sourceReview,
      },
      {
        id: "walkthrough-master-01",
        kind: "walkthrough-master",
        publicLabel: "Reviewed walkthrough upload master",
        sha256: "a".repeat(64),
        durationSeconds: 100,
        frameRate: 30,
        frameCount: 3000,
        liveCapture: false,
        ...sourceReview,
      },
    ],
    deliveries: {
      "coop-hero-loop.webm": deliveryReview(hashes.get("coop-hero-loop.webm"), {
        sourceId: "proof-take-01",
        sourceIn: "00:00:05.000",
        sourceOut: "00:00:12.000",
      }),
      "coop-hero-loop.mp4": deliveryReview(hashes.get("coop-hero-loop.mp4"), {
        sourceId: "proof-take-01",
        sourceIn: "00:00:05.000",
        sourceOut: "00:00:12.000",
      }),
      "coop-hero-poster.webp": deliveryReview(hashes.get("coop-hero-poster.webp"), {
        sourceId: "proof-take-01",
        sourceTimecode: "00:00:06.000",
        sourceFrame: 180,
      }),
      "living-room-wide.webp": deliveryReview(hashes.get("living-room-wide.webp"), {
        sourceId: "room-photo-01",
        sourceFrame: "original-still",
      }),
      "coop-picker.webp": deliveryReview(hashes.get("coop-picker.webp"), {
        sourceId: "picker-still-01",
        sourceFrame: "original-still",
      }),
      "coop-walkthrough.en.vtt": deliveryReview(hashes.get("coop-walkthrough.en.vtt"), {
        sourceId: "walkthrough-master-01",
      }),
      "coop-walkthrough-transcript.md": deliveryReview(hashes.get("coop-walkthrough-transcript.md"), {
        sourceId: "walkthrough-master-01",
      }),
    },
  };
  return { manifest, hashes };
}

test("public media inventory rejects unreviewed files, nesting and symlinks", (t) => {
  const mediaRoot = fs.mkdtempSync(path.join(os.tmpdir(), "eclipse-media-inventory-"));
  t.after(() => fs.rmSync(mediaRoot, { recursive: true, force: true }));
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "reviewed deliveries only\n");
  assert.deepEqual(inventoryMediaDirectory(mediaRoot).errors, []);
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "a".repeat(100 * 1024));
  assert.deepEqual(inventoryMediaDirectory(mediaRoot).errors, []);
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "a".repeat(100 * 1024 + 1));
  assert.ok(inventoryMediaDirectory(mediaRoot).errors.some((error) => error.includes("102401 bytes exceeds 102400")));
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "Copy from /root/private-recording/source.mov\n");
  assert.ok(inventoryMediaDirectory(mediaRoot).errors.some((error) =>
    error.includes("README.md") && error.includes("local filesystem path")));
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "Preview at http://localhost:8080/review\n");
  assert.ok(inventoryMediaDirectory(mediaRoot).errors.some((error) =>
    error.includes("README.md") && error.includes("non-public URL")));
  fs.writeFileSync(path.join(mediaRoot, "README.md"), "reviewed deliveries only\n");
  fs.writeFileSync(path.join(mediaRoot, "raw-original.mov"), "not public\n");
  fs.mkdirSync(path.join(mediaRoot, "sidecars"));
  fs.symlinkSync(path.join(mediaRoot, "README.md"), path.join(mediaRoot, "linked-readme"));
  const errors = inventoryMediaDirectory(mediaRoot).errors;
  assert.ok(errors.some((error) => error.includes("raw-original.mov") && error.includes("allowlist")));
  assert.ok(errors.some((error) => error.includes("sidecars") && error.includes("nested")));
  assert.ok(errors.some((error) => error.includes("linked-readme") && error.includes("symbolic")));
});

test("public walkthrough URL rejects local, reserved and literal-IP targets", () => {
  assert.equal(isPublicHttpsUrl("https://www.youtube.com/watch?v=AbCdEfGhI12"), true);
  for (const value of [
    "http://www.youtube.com/watch?v=AbCdEfGhI12",
    "https://foo.localhost/watch",
    "https://foo.localhost./watch",
    "https://device.home.arpa/watch",
    "https://127.0.0.1/watch",
    "https://[::1]/watch",
    "https://[fe80::1]/watch",
    "https://video.example.com/watch",
    "https://video.example.com./watch",
    "https://video.example/watch",
    "https://intranet/watch",
  ]) assert.equal(isPublicHttpsUrl(value), false, value);
});

test("WebVTT parser accepts ordered non-overlapping cues", () => {
  const cues = parseWebVtt(
    "WEBVTT\n\n00:00.000 --> 00:02.000\nTwo PCs appear.\n\n00:02.000 --> 00:04.500\n[Controllers click] Both players move.\n",
    100,
  );
  assert.equal(cues.length, 2);
  assert.equal(cues[1].end, 4.5);
  const positioned = parseWebVtt(
    "WEBVTT\n\n00:00.000 --> 00:01.000 align:center position:50% line:90% size:80% vertical:lr\nCaption\n",
    100,
  );
  assert.equal(positioned.length, 1);
});

test("WebVTT parser rejects reversed, overlapping, placeholder and out-of-range cues", () => {
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:10.000 --> 00:01.000\nBad timing\n", 100),
    /later than cue start/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:03.000\nFirst\n\n00:02.000 --> 00:04.000\nSecond\n", 100),
    /overlap/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000\n[FILL]\n", 100),
    /placeholder/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000\n{{CAPTION_TEXT}}\n", 100),
    /placeholder/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000\nTKTK\n", 100),
    /placeholder/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\nNOTE\n/home/operator/private.mov\n\n00:00.000 --> 00:01.000\nCaption\n", 100),
    /local filesystem path/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\noperator@example.invalid\n00:00.000 --> 00:01.000\nCaption\n", 100),
    /email address|reserved example URL/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n01:39.000 --> 01:41.000\nToo late\n", 100),
    /after the 100s walkthrough/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n01:39.000 --> 01:40.001\nOne millisecond late\n", 100),
    /after the 100s walkthrough/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000\nFirst\n00:01.000 --> 00:02.000\nSecond\n", 100),
    /blank cue separator is missing/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000 00:00.500 --> 00:02.000\nJoined cue\n", 100),
    /invalid cue timing line/,
  );
  assert.throws(
    () => parseWebVtt("WEBVTT\n\n00:00.000 --> 00:01.000 align:banana\nBad setting\n", 100),
    /invalid cue setting/,
  );
});

test("MP4 parser reads top-level atoms rather than matching payload text", () => {
  const valid = Buffer.concat([
    mp4Atom("ftyp", Buffer.from("isom")),
    mp4Atom("moov"),
    mp4Atom("mdat", Buffer.from("payload")),
  ]);
  assert.deepEqual(parseMp4TopLevelAtoms(valid).map((atom) => atom.type), ["ftyp", "moov", "mdat"]);
  assert.equal(validateMp4FastStart(valid).length, 3);

  const payloadOnly = Buffer.concat([
    mp4Atom("ftyp", Buffer.from("isom")),
    mp4Atom("mdat", Buffer.from("moov inside payload")),
  ]);
  assert.deepEqual(parseMp4TopLevelAtoms(payloadOnly).map((atom) => atom.type), ["ftyp", "mdat"]);
  assert.throws(() => validateMp4FastStart(payloadOnly), /expected one top-level moov/);
  assert.throws(
    () => validateMp4FastStart(Buffer.concat([mp4Atom("ftyp"), mp4Atom("mdat"), mp4Atom("moov")])),
    /not fast-started/,
  );
  assert.throws(
    () => validateMp4FastStart(Buffer.concat([
      mp4Atom("ftyp"),
      mp4Atom("moov"),
      mp4Atom("mdat"),
      mp4Atom("uuid", Buffer.from("0123456789abcdef/home/example/private-source")),
    ])),
    /unexpected top-level atoms remain: uuid/,
  );
  assert.throws(
    () => validateMp4FastStart(Buffer.concat([
      mp4Atom("ftyp"),
      mp4Atom("moov"),
      mp4Atom("free", Buffer.from("/home/example/private-source")),
      mp4Atom("mdat"),
    ])),
    /free\/skip padding atoms must have no payload/,
  );
  assert.throws(
    () => validateMp4FastStart(Buffer.concat([
      mp4Atom("ftyp"),
      mp4Atom("moov", Buffer.concat([
        mp4Atom("mvhd"),
        mp4Atom("uuid", Buffer.from("0123456789abcdef/root/private-recording/source.mov")),
      ])),
      mp4Atom("mdat"),
    ])),
    /MP4 bytes contains a local filesystem path/,
  );
  assert.throws(
    () => validateMp4FastStart(Buffer.concat([
      mp4Atom("ftyp"),
      mp4Atom("moov"),
      mp4Atom("mdat", Buffer.from("encoded-data/root/private-recording/source.mov")),
    ])),
    /MP4 bytes contains a local filesystem path/,
  );
  assert.throws(() => parseMp4TopLevelAtoms(Buffer.from("short")), /truncated/);
});

test("WebP parser requires one still frame and rejects public metadata chunks", () => {
  const valid = webpFile(webpChunk("VP8L", Buffer.from([0])));
  assert.equal(inspectWebP(valid).imageFrameCount, 1);
  assert.throws(
    () => inspectWebP(webpFile(webpChunk("VP8L", Buffer.from([0])), webpChunk("EXIF", Buffer.from([1])))),
    /forbidden WebP chunks/,
  );
  assert.throws(
    () => inspectWebP(webpFile(webpChunk("VP8L", Buffer.from([0])), webpChunk("PRIV", Buffer.from("\/home\/operator\/source")))),
    /unknown WebP chunks are not allowed/,
  );
  assert.throws(
    () => inspectWebP(webpFile(webpChunk("VP8L", Buffer.from([0])), webpChunk("VP8L", Buffer.from([1])))),
    /one still-image payload/,
  );
  const vp8x = Buffer.alloc(10);
  vp8x[0] = 0x20;
  assert.throws(
    () => inspectWebP(webpFile(webpChunk("VP8X", vp8x), webpChunk("VP8L", Buffer.from([0])))),
    /flags must all be zero/,
  );
  assert.throws(
    () => inspectWebP(webpFile(
      webpChunk("VP8L", Buffer.from([0])),
      webpChunk("VP8X", Buffer.concat([Buffer.alloc(10), Buffer.from("/home/operator/source")])),
    )),
    /VP8X chunk must be exactly 10 bytes|accepted opaque still-image layout/,
  );
  assert.throws(
    () => inspectWebP(webpFile(webpChunk("VP8L", Buffer.from("encoded-data/root/private-source.mov")))),
    /WebP bytes contains a local filesystem path/,
  );
});

test("rotation validation rejects nonnumeric and nonzero metadata", () => {
  assert.equal(hasInvalidRotationMetadata({}), false);
  assert.equal(hasInvalidRotationMetadata({ tags: { rotate: "0" } }), false);
  assert.equal(hasInvalidRotationMetadata({ tags: { rotate: "sideways" } }), true);
  assert.equal(hasInvalidRotationMetadata({ tags: { rotate: "90" } }), true);
  assert.equal(hasInvalidRotationMetadata({ side_data_list: [{ rotation: Number.NaN }] }), true);
  assert.equal(hasInvalidRotationMetadata({
    side_data_list: [{ side_data_type: "Display Matrix", displaymatrix: "identity-with-vertical-flip", rotation: 0 }],
  }), true);
});

test("media stream validation rejects audio, subtitle, data and unknown streams", () => {
  assert.deepEqual(unexpectedNonVideoStreamTypes([{ codec_type: "video" }]), []);
  assert.deepEqual(
    unexpectedNonVideoStreamTypes([
      { codec_type: "video" },
      { codec_type: "audio" },
      { codec_type: "subtitle" },
      { codec_type: "data" },
      {},
      { codec_type: "audio" },
    ]),
    ["audio", "subtitle", "data", "unknown"],
  );
  assert.equal(hasUnexpectedChapters([]), false);
  assert.equal(hasUnexpectedChapters([{ id: 0, tags: { title: "/root/private-recording.mov" } }]), true);
});

test("technical metadata is allowlisted and every value receives a privacy scan", () => {
  assert.deepEqual(
    validateTechnicalMetadata(
      { major_brand: "isom", minor_version: "512", compatible_brands: "isomiso2avc1mp41", encoder: "Lavf62.3.100" },
      { language: "und", handler_name: "VideoHandler", vendor_id: "[0][0][0][0]", encoder: "Lavc62.11.100 libx264" },
    ),
    [],
  );
  const privateHandler = validateTechnicalMetadata({}, { handler_name: "/home/operator/private-capture" });
  assert.ok(privateHandler.some((error) => error.includes("local filesystem path")));
  assert.ok(privateHandler.some((error) => error.includes("unexpected value")));
  assert.ok(validateTechnicalMetadata({}, { title: "Launch clip" }).some((error) => error.includes("not an allowed")));
});

test("binary privacy scan catches high-confidence leaks without treating compressed TK bytes as a placeholder", () => {
  assert.deepEqual(binaryPrintableSafetyProblems([Buffer.from("random-TK'compressed-bytes")]), []);
  assert.ok(binaryPrintableSafetyProblems([Buffer.from("encoded-data/root/private-recording/source.mov")])
    .includes("contains a local filesystem path"));
  assert.ok(binaryPrintableSafetyProblems([Buffer.from("metadata producer@example.invalid")])
    .includes("contains an email address"));
});

test("video measurements reject non-finite, non-positive and non-integer facts", () => {
  assert.equal(validateVideoMeasurements("clip", { duration: 7, frameRate: 60, frameCount: 420 }).length, 0);
  assert.equal(validateVideoMeasurements("clip", { duration: 8, frameRate: 30000 / 1001, frameCount: 240 }).length, 0);
  assert.ok(validateVideoMeasurements("clip", { duration: 8.001, frameRate: 30, frameCount: 240 }).some((error) =>
    error.includes("6–8s contract")));
  assert.ok(validateVideoMeasurements("clip", { duration: 7, frameRate: 29.5, frameCount: 210 }).some((error) =>
    error.includes("nominal 30–60fps")));
  assert.ok(validateVideoMeasurements("clip", { duration: 7, frameRate: 60.5, frameCount: 424 }).some((error) =>
    error.includes("nominal 30–60fps")));
  const failures = validateVideoMeasurements("clip", {
    duration: Number.NaN,
    frameRate: 0,
    frameCount: Number.NaN,
  });
  assert.equal(failures.length, 3);
});

test("ffprobe status zero does not hide decoder/container errors", () => {
  assert.equal(probeProcessFailed({ status: 0, stderr: "" }), false);
  assert.equal(probeProcessFailed({ status: 0, stderr: "Canvas dimensions are already set\n" }), true);
  assert.equal(probeProcessFailed({ status: 1, stderr: "" }), true);
});

test("decoded frame timelines allow only a shared monotonic timeline", () => {
  assert.deepEqual(compareFrameTimelines([10, 10.033, 10.067], [0, 0.033, 0.067]), []);
  assert.match(compareFrameTimelines([0, 0.033, 0.067], [0, 0.033, 0.08])[0], /differ/);
  assert.match(compareFrameTimelines([0, 0.033, 0.02], [0, 0.033, 0.067])[0], /strictly increasing/);
});

test("SSIM summary parser returns the final aggregate score", () => {
  assert.equal(parseSsim("SSIM Y:0.98 U:0.99 V:0.99 All:0.987654 (18.9)"), 0.987654);
  assert.ok(Number.isNaN(parseSsim("no score")));
  assert.deepEqual(
    parseSsimFrameStats("n:1 Y:0.99 U:0.99 V:0.99 All:0.991000 (20.4)\nn:2 Y:0.98 U:0.99 V:0.99 All:0.985000 (18.2)\n"),
    [{ number: 1, all: 0.991 }, { number: 2, all: 0.985 }],
  );
});

test("media manifest validates builds, public sources, hashes, lineage and reviews", () => {
  const { manifest, hashes } = sampleManifest();
  assert.deepEqual(validateManifest(manifest, hashes), []);

  const broken = structuredClone(manifest);
  broken.deliveries["coop-hero-loop.mp4"].sha256 = "0".repeat(64);
  broken.deliveries["coop-hero-poster.webp"].lineage[0].sourceId = "picker-still-01";
  const errors = validateManifest(broken, hashes);
  assert.ok(errors.some((error) => error.includes("does not match")));
  assert.ok(errors.some((error) => error.includes("same verified live run")));

  const unsafe = structuredClone(manifest);
  unsafe.sourceInventory[0].publicLabel = "Raw take at /home/operator/private.mov";
  unsafe.deliveries["coop-picker.webp"].privacyReviewed = false;
  const unsafeErrors = validateManifest(unsafe, hashes);
  assert.ok(unsafeErrors.some((error) => error.includes("local filesystem path")));
  assert.ok(unsafeErrors.some((error) => error.includes("privacyReviewed")));

  const placeholderSource = structuredClone(manifest);
  placeholderSource.sourceInventory[0].publicLabel = "CHANGEME";
  assert.ok(validateManifest(placeholderSource, hashes).some((error) => error.includes("placeholder")));

  for (const privateUrl of ["https://localhost/review", "https://capture.local/review", "https://home.arpa/review", "https://intranet/review"]) {
    const privateSourceUrl = structuredClone(manifest);
    privateSourceUrl.sourceInventory[0].publicLabel = `Reviewed at ${privateUrl}`;
    assert.ok(validateManifest(privateSourceUrl, hashes).some((error) => error.includes("non-public URL")), privateUrl);
  }

  const exampleUrl = structuredClone(manifest);
  exampleUrl.walkthrough.url = "https://video.example.com/watch/demo";
  assert.ok(validateManifest(exampleUrl, hashes).some((error) => error.includes("public HTTPS URL")));

  const frameMissing = structuredClone(manifest);
  delete frameMissing.deliveries["coop-hero-poster.webp"].lineage[0].sourceFrame;
  assert.ok(validateManifest(frameMissing, hashes).some((error) => error.includes("nonnegative safe integer sourceFrame")));

  const contradictoryFrame = structuredClone(manifest);
  contradictoryFrame.deliveries["coop-hero-poster.webp"].lineage[0].sourceFrame = 10;
  assert.ok(validateManifest(contradictoryFrame, hashes).some((error) => error.includes("maps to frame 180")));
  const unsafeFrame = structuredClone(manifest);
  unsafeFrame.deliveries["coop-hero-poster.webp"].lineage[0].sourceFrame = 1e100;
  assert.ok(validateManifest(unsafeFrame, hashes).some((error) => error.includes("safe integer")));

  const timedStill = structuredClone(manifest);
  timedStill.sourceInventory.find((source) => source.id === "room-photo-01").durationSeconds = Number.NaN;
  timedStill.sourceInventory.find((source) => source.id === "room-photo-01").frameRate = 30;
  assert.ok(validateManifest(timedStill, hashes).some((error) => error.includes("still sources must omit")));

  const malformedVersion = structuredClone(manifest);
  malformedVersion.builds.eclipse.version = "1";
  assert.ok(validateManifest(malformedVersion, hashes).some((error) => error.includes("dot-separated numeric")));
});

test("transcript requires structured reviewed content and exact public build record", () => {
  const { manifest } = sampleManifest();
  const labels = { eclipse: "Eclipse", umbraHostA: "Umbra host A", umbraHostB: "Umbra host B" };
  const builds = Object.entries(manifest.builds)
    .map(([name, build]) => `${labels[name]}: version ${build.version}; commit ${build.commit}; artifact SHA-256 ${build.artifactSha256}.`)
    .join("\n");
  const transcript = `# Eclipse/Umbra co-op walkthrough

## Build record

${builds}

## Narration and sound cues

00:00 — Two gaming PCs appear on one shared screen. [Controller buttons click.] Both players move independently while the narrator explains the live composition.

## Visual description

The television shows two distinct live panes. The first player moves only the left pane, the second player moves only the right pane, and then both move together.

## Public links

Watch the reviewed film at ${manifest.walkthrough.url}. The project source, setup guide and complete acknowledgements are linked from the walkthrough page.

## Credits

Moonlight and Sunshine provide the streaming foundations. Moonlight TV, Aurora and Apollo are credited as the upstream projects from which this work descends. Complete creator and licence details remain in the acknowledgements.
`;
  assert.deepEqual(validateTranscript(transcript, manifest), []);
  const templateBuilds = Object.entries(manifest.builds)
    .map(([name, build]) =>
      `- ${labels[name]} version and commit: \`${build.version}\` / \`${build.commit}\`; artifact SHA-256 \`${build.artifactSha256}\`.`)
    .join("\n");
  assert.deepEqual(validateTranscript(transcript.replace(builds, templateBuilds), manifest), []);

  const filler = `# Transcript\n${"x".repeat(500)}`;
  assert.ok(validateTranscript(filler, manifest).some((error) => error.includes("Build record")));
  assert.ok(validateTranscript(`${transcript}\nTODO`, manifest).some((error) => error.includes("placeholder")));
  assert.ok(validateTranscript(`${transcript}\n[ FILL ]`, manifest).some((error) => error.includes("placeholder")));
  assert.ok(validateTranscript(`${transcript}\nFIXME`, manifest).some((error) => error.includes("placeholder")));
  assert.ok(validateTranscript(`${transcript}\nInternal review: https://device.local/capture`, manifest)
    .some((error) => error.includes("non-public URL")));
  assert.ok(
    validateTranscript(transcript.replaceAll(manifest.walkthrough.url, "https://video.example.org/watch/other"), manifest)
      .some((error) => error.includes("hosted walkthrough URL")),
  );
  const fenced = `# Visible title\n\n\`\`\`markdown\n${transcript}\n\`\`\`\n${"ordinary prose ".repeat(40)}`;
  assert.ok(validateTranscript(fenced, manifest).some((error) => error.includes("Build record")));
  const commented = `# Visible title\n\n<!--\n${transcript}\n-->\n${"ordinary prose ".repeat(40)}`;
  assert.ok(validateTranscript(commented, manifest).some((error) => error.includes("Build record")));

  const shortVersionManifest = structuredClone(manifest);
  shortVersionManifest.builds.eclipse.version = "1";
  const privateIpTranscript = transcript.replace("version 1.2.3.4", "version 1") + "\nCaptured from 192.168.1.1.\n";
  assert.ok(validateTranscript(privateIpTranscript, shortVersionManifest).some((error) => error.includes("IP address")));

  const ipVersionManifest = structuredClone(manifest);
  ipVersionManifest.builds.eclipse.version = "192.168.1.1";
  const ipVersionTranscript = transcript.replace("version 1.2.3.4", "version 192.168.1.1") +
    "\nCaptured from private host 192.168.1.1.\n";
  assert.ok(validateTranscript(ipVersionTranscript, ipVersionManifest).some((error) => error.includes("IP address")));
});
