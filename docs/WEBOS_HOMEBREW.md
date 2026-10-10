# webOS Homebrew publication

> [!WARNING]
> **Publication is blocked.** Do not submit the current catalog YAML or publish a release merely to test this guide. The inherited metadata still points to the previous Aurora repository, artwork, release manifest and recipient; the current IPK also needs complete release-specific licence, notice and corresponding-source material.

The official catalog is [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo), with submission guidance at [repo.webosbrew.org](https://repo.webosbrew.org/submit). This project will use that normal review path only after the public-launch checklist is complete.

## Gates before submission

1. Apply and verify the chosen **Eclipse** public identity across the generated package and catalog metadata, and register/verify every packaging-identity parameter. Keep the installed application ID and compatibility paths unchanged unless a separately measured migration is approved.
2. Publish the final source repository and a reviewed release containing the webOS package, manifest, checksums, licence/notice bundle and exact corresponding-source reference.
3. Replace every inherited repository, icon, badge, manifest and maintainer/funding reference in `deploy/webosbrew/` and `cmake/AresPackage.cmake` with final project-owned values.
4. Test both a fresh install and the first stream of the live and beta application identities on the current webOS release. The first launch matters because the media plugin state is rebuilt per application identity.
5. Verify the package on the real TV display, not only from counters or install success.
6. Confirm rollback from a retained known-good package and record the tested source commit.
7. Check credits, media rights, support disclosures and every link while signed out of GitHub.

## Submission procedure after the gates clear

1. Generate a fresh catalog entry from the final public repository and release rather than editing the inherited file piecemeal.
2. Confirm the icon and detail-icon URLs return the final approved artwork from the public default branch.
3. Confirm the manifest URL returns the exact reviewed release manifest in a signed-out browser.
4. Install through the same manifest/catalog path a consumer would use and run the fresh-install/first-stream test again.
5. Fork `webosbrew/apps-repo`, add the reviewed package metadata under its `packages/` directory, and open a pull request following the current upstream submission guide.
6. Keep the catalog description factual: co-op first, clear engineering-preview status, tested platforms only, direct lineage credit, and no unsupported performance or compatibility promises.

## Maintenance

Treat each public package as a release artifact, not as a pointer to a moving internal build. Before updating the catalog, re-run the artifact notices/source audit, link checks, fresh-install test and on-screen verification. If an application identity or manifest filename ever changes, plan and document migration rather than silently replacing it.

The current gated metadata remains at [`deploy/webosbrew/com.aurora.gamestream.yml`](../deploy/webosbrew/com.aurora.gamestream.yml) only as a historical input for the later regeneration pass.
