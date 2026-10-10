# Client dependency patches

Eclipse pins these submodules to commits available from their public upstream repositories. The
client needs a small set of changes that have not landed upstream, so
`scripts/bootstrap_client_deps.sh` applies the patches in this directory before every Windows,
Linux, or webOS build.

| Dependency | Public base | Patch |
| --- | --- | --- |
| `core/moonlight-common-c` | `c86e0537d11566f9825d6323711ec61e3e702365` | `moonlight-common-c.patch` |
| `third_party/ss4s` | `9cbad6a97b951aaf8d957b84f14b652ddb3dd5d2` | `ss4s.patch` |
| `third_party/commons` | `ed351407d965adda289f4eec53ce826a4d19f111` | `commons.patch` |

The bootstrap is idempotent: it accepts an exact already-applied patch, applies it to the pinned
clean base, and otherwise stops without resetting local work. Keep the gitlink, base revision in
the bootstrap, and patch together when updating a dependency. Patch files must include new files
and file-mode changes as well as tracked edits (`git diff --binary --full-index`).

