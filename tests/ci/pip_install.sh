#!/usr/bin/env bash
# Install pinned PyPI packages from the CI cache, without the index.
#
# The same reason as apt_install.sh: the network step is what hangs, not the
# install. pip resolves against the index on every run even when the wheel is
# already on the runner, and the index has stalled like the apt mirror. So the
# wheels of a complete set live in $1, installs are --no-index --find-links,
# and only a cache miss touches PyPI - time-boxed, with retries.
#
# Wheel-only on purpose: the cache is a directory of .whl files, so a miss can
# never turn into a source build on the runner.
#
# Usage: pip_install.sh CACHE_DIR PACKAGE...
set -euo pipefail

trap 'echo "pip_install.sh: failed at line $LINENO" >&2' ERR

cache=${1:?usage: pip_install.sh CACHE_DIR PACKAGE...}
shift
packages=("$@")
if [ "${#packages[@]}" -eq 0 ]; then
    echo "pip_install.sh: no packages" >&2
    exit 2
fi

mkdir -p "$cache"

pip_install() {
    python3 -m pip install --no-index --find-links "$cache" \
        --disable-pip-version-check --no-input "$@"
}

# Offline path. pip itself decides whether the requirement is already met, so
# this is a no-op on a runner that ships the package (numpy often is).
if compgen -G "$cache/*.whl" >/dev/null; then
    echo "pip: installing from cache: ${packages[*]}"
    if pip_install "${packages[@]}"; then
        echo "pip: cache hit"
        exit 0
    fi
    echo "pip: cached set is incomplete, going to the index"
fi

# Miss: fetch the wheels once (dependencies included), keep them, install
# offline. timeout is the backstop for a stalled index; pip's own retries are
# bounded too.
timeout 300 python3 -m pip download --only-binary=:all: --dest "$cache" \
    --disable-pip-version-check --no-input --timeout 20 --retries 2 \
    "${packages[@]}"
pip_install "${packages[@]}"
echo "pip: installed from the index; the next run is offline"
