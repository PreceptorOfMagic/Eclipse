#!/usr/bin/env bash
# Initialise only Eclipse's client build dependencies and apply the small,
# repository-owned deltas that are not available in their public upstreams.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

if ! command -v git >/dev/null 2>&1; then
    echo "Error: Git is required to initialise client dependencies." >&2
    exit 1
fi
if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "Error: scripts/bootstrap_client_deps.sh must run inside an Eclipse Git checkout." >&2
    exit 1
fi

plain_deps=(
    third_party/lvgl
    third_party/libmicrodns
    third_party/Unity
)

patched_deps=(
    core/moonlight-common-c
    third_party/ss4s
    third_party/commons
)

expected_gitlink() {
    local repository=$1
    local path=$2
    git -C "${repository}" ls-files --stage -- "${path}" | awk '$1 == 160000 { print $2; exit }'
}

ensure_plain_submodule() {
    local repository=$1
    local path=$2
    local display_path=$3
    local expected current

    expected="$(expected_gitlink "${repository}" "${path}")"
    if [[ -z "${expected}" ]]; then
        echo "Error: no pinned Git revision is registered for ${display_path}." >&2
        exit 1
    fi

    # An empty submodule directory sits inside the superproject, so `git -C
    # <empty-dir> rev-parse --git-dir` incorrectly succeeds by walking upward.
    # A checked-out submodule always owns a .git file (or directory) at its root.
    if [[ ! -e "${repository}/${path}/.git" ]]; then
        git -C "${repository}" submodule update --init -- "${path}"
    fi

    current="$(git -C "${repository}/${path}" rev-parse HEAD)"
    if [[ "${current}" == "${expected}" ]]; then
        return
    fi

    if [[ -n "$(git -C "${repository}/${path}" status --porcelain)" ]]; then
        echo "Error: ${display_path} is modified at ${current}, but Eclipse pins ${expected}." >&2
        echo "Preserve or commit that work before updating the dependency." >&2
        exit 1
    fi

    git -C "${repository}" submodule update --init -- "${path}"
    current="$(git -C "${repository}/${path}" rev-parse HEAD)"
    if [[ "${current}" != "${expected}" ]]; then
        echo "Error: ${display_path} resolved to ${current}, expected ${expected}." >&2
        exit 1
    fi
}

ensure_patched_submodule() {
    local path=$1
    local base_revision=$2
    local patch_path=$3
    local patch_abs="${PROJECT_ROOT}/${patch_path}"
    local current

    if [[ ! -f "${patch_abs}" ]]; then
        echo "Error: required dependency patch is missing: ${patch_path}" >&2
        exit 1
    fi

    if [[ ! -e "${path}/.git" ]]; then
        git submodule update --init -- "${path}"
    fi

    # This also supports existing developer checkouts whose private branch
    # already contains the same source delta. No private remote is consulted.
    if git -C "${path}" apply --reverse --check "${patch_abs}" >/dev/null 2>&1; then
        echo "Client dependency ready: ${path}"
        return
    fi

    current="$(git -C "${path}" rev-parse HEAD)"
    if [[ "${current}" != "${base_revision}" ]]; then
        echo "Error: ${path} is at ${current}; expected public base ${base_revision}." >&2
        echo "The Eclipse patch was not already present, so this checkout was left untouched." >&2
        exit 1
    fi
    if [[ -n "$(git -C "${path}" status --porcelain)" ]]; then
        echo "Error: ${path} has local changes and cannot be patched reproducibly." >&2
        echo "The checkout was left untouched." >&2
        exit 1
    fi

    git -C "${path}" apply --check "${patch_abs}"
    git -C "${path}" apply "${patch_abs}"
    if ! git -C "${path}" apply --reverse --check "${patch_abs}" >/dev/null 2>&1; then
        echo "Error: verification failed after applying ${patch_path}." >&2
        exit 1
    fi
    echo "Applied ${patch_path} to ${path}"
}

# Sync only client inputs.
git submodule sync -- "${plain_deps[@]}" "${patched_deps[@]}"

for dependency in "${plain_deps[@]}"; do
    ensure_plain_submodule "${PROJECT_ROOT}" "${dependency}" "${dependency}"
done

ensure_patched_submodule \
    core/moonlight-common-c \
    c86e0537d11566f9825d6323711ec61e3e702365 \
    patches/client-deps/moonlight-common-c.patch
ensure_patched_submodule \
    third_party/ss4s \
    9cbad6a97b951aaf8d957b84f14b652ddb3dd5d2 \
    patches/client-deps/ss4s.patch
ensure_patched_submodule \
    third_party/commons \
    ed351407d965adda289f4eec53ce826a4d19f111 \
    patches/client-deps/commons.patch

# Moonlight's sole nested dependency is public and pinned by its own gitlink.
git -C core/moonlight-common-c submodule sync -- enet
ensure_plain_submodule \
    "${PROJECT_ROOT}/core/moonlight-common-c" \
    enet \
    core/moonlight-common-c/enet

echo "Eclipse client dependencies are ready."
