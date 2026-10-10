# Building the project source snapshot

This archive contains the Eclipse project and its pinned Git submodules. The
repository-owned client dependency patches are already applied. It includes
`res/` and other project assets used by the platform build recipes. It omits Git
history, internal run logs, captured test sessions, generated Python bytecode and
the old compiled TV probe fixture.

`SOURCE-BUNDLE.json` records the project commit, dependency commits, exact patch
hashes, expected patched Git trees, observed working Git diff hashes, verified
patch additions and every archived file's SHA-256 and Git mode. Dependency source
bytes are reconstructed from pinned Git objects and committed patches; the
builder's working files must match before an archive can be created.

This is **one source component**, not a complete corresponding-source release for
all packaged libraries. The matching external-library source archives, their
notices, build recipes, toolchains and platform SDKs must be supplied separately.
See the binary package's source manifest and the dependency recipes under
`scripts/linux/portable`, `scripts/windows` and `cmake`. Merely possessing this
archive does not prove those external inputs are available or licence-complete.

## Git-free builds

Extract into a new directory, verify the source manifest's file hashes, and leave
the pre-applied dependency trees intact. Do not run
`scripts/bootstrap_client_deps.sh`: it initializes and patches Git checkouts and
requires Git metadata that is deliberately absent here.

Provision the external dependencies for your platform, then configure CMake
directly. Use the compiler/toolchain, dependency prefixes and module selections
from the matching platform recipe, adapted to your local dependency locations.
The basic command sequence, once those dependencies are discoverable, is:

```sh
cmake -S . -B build/local
cmake --build build/local --target moonlight
```

The top-level source does not require Git metadata to compile: its diagnostic
build-commit label falls back to `unknown`. The archive manifest preserves the
real source identity. This direct build procedure is not a promise that the
Git-oriented portable release wrappers or webOS packaging metadata generator run
unchanged in an extracted archive. Producing a redistributable installer also
requires the platform's dependency staging, notices, payload audits and lifecycle
tests; building the executable alone does not perform those steps.
