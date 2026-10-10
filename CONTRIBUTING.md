# Contributing

Thanks for helping improve Eclipse, Umbra, their two-host co-op integration, documentation, or release tooling. Eclipse/Umbra is still an engineering preview, so contributions should favour clear behaviour and compatibility over unnecessary identity churn.

## Start with the right report

- Use the structured [bug report](https://github.com/PreceptorOfMagic/eclipse/issues/new?template=bug-report.yml) for reproducible failures.
- Use the [feature request](https://github.com/PreceptorOfMagic/eclipse/issues/new?template=feature-request.yml) for a user problem that needs new behaviour.
- If a problem reproduces in an unmodified upstream project, report it to that project rather than presenting it as a fork-specific failure.
- Never post pairing codes, credentials, account data, public IP addresses, host identifiers, or unredacted logs/screenshots.

The repository is private during release preparation, so these links become usable when the source is published.

## Before changing code

1. Verify the hypothesis against the current source. Keep the change as small as the behaviour allows.
2. Look up every runtime/configuration parameter with `python3 scripts/ctx/param.py <name>`. Exit code 0 is the only green result; not-found and withdrawn parameters stop the change until their ownership and evidence are resolved.
3. The public application names are fixed: Eclipse is the client and Umbra is the host. Replace user-facing Aurora/Apollo branding accordingly, but retain inherited Moonlight/Sunshine names and compatibility identifiers or data paths unless the change explicitly includes a measured migration.
4. New third-party components must have public source and a licence compatible with the affected artifact. Record the exact revision, licence, notices, build inputs and source-offer implications.
5. Do not add downloaded binaries when an auditable source build is practical. Signed kernel drivers are the narrow exception and still require hash and Authenticode verification.

## Evidence expected

- Describe the user-visible prediction before the test and the measurement that would change if the patch took effect.
- Streaming results must come from a live host encode, not prerecorded playback.
- For anything affecting video or composition, include a redacted screenshot or photograph of the actual client display and say what it shows—even when it contradicts healthy counters.
- Co-op work should identify both host roles, layout, codecs/hardware paths, client platform and exact tested commits without publishing rig-specific addresses or identifiers.
- Close and verify the test harness on both hosts after a run or abort. A leftover full-screen pattern or receiver invalidates later tests and disrupts the physical host display.
- Run the smallest relevant build/check first, then broaden verification in proportion to the risk.

## Documentation and public-site changes

- Keep the canonical consumer story co-op-first while preserving factual Moonlight → Moonlight TV → Aurora and Sunshine → Apollo lineage.
- Separate measured support from planned or experimental behaviour. Do not turn an internal test into a broad compatibility claim.
- Website changes belong in the Eclipse-Umbra Project Hub repository, not here.
- Use original or cleared media, include alt text, captions and a transcript, and never repurpose diagnostic logs or captures as promotional material.
- Acknowledgements do not replace the licence, copyright, NOTICE, corresponding-source or relinking material required inside a released artifact.

## Pull requests

A focused pull request should include:

- the problem and scope;
- the upstream/fork relationship when relevant;
- files and behaviours changed;
- build and test commands with results;
- visual evidence for UI/video changes;
- known limitations or untested paths; and
- any added dependency, licence, notice or corresponding-source impact.

Avoid unrelated formatting or cleanup in the same change. Existing uncommitted work belongs to its author; do not overwrite or discard it.

By contributing, you agree that your contribution is provided under the repository’s applicable open-source licence and that you have the right to submit it. The client derivative declaration is recorded in [COPYRIGHT](COPYRIGHT); exact artifact notices and corresponding-source obligations are still being reconciled before public distribution, as listed in [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).
