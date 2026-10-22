#!/usr/bin/env bash
# Install apt packages from the CI cache, and never let the archive hang a job.
#
# Plain "apt-get install" hangs: the Ubuntu mirror has stalled for 14+ minutes
# with no output. Caching /var/cache/apt/archives alone does not help, for two
# reasons:
#
#   * the runner image runs docker-clean, which deletes the .debs as apt exits,
#     so the "copy the archives" step copies nothing;
#   * apt-get update - the step that actually stalls - still runs on every job,
#     even when the packages are already on disk.
#
# So this keeps the .debs of a complete package set (dependencies included) in
# $1, installs them with dpkg when the cache hits, and goes to the network only
# on a miss. Every network call is time-boxed: a stalled mirror fails the job
# in minutes instead of hanging it, and the next run retries.
#
# Usage: apt_install.sh CACHE_DIR PACKAGE...
set -euo pipefail

# A silent exit is what makes a CI failure expensive: say where it stopped.
trap 'echo "apt_install.sh: failed at line $LINENO" >&2' ERR

cache=${1:?usage: apt_install.sh CACHE_DIR PACKAGE...}
shift
packages=("$@")
if [ "${#packages[@]}" -eq 0 ]; then
    echo "apt_install.sh: no packages" >&2
    exit 2
fi

installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'
}

all_installed() {
    local p
    for p in "${packages[@]}"; do
        installed "$p" || return 1
    done
}

if all_installed; then
    echo "apt: already installed: ${packages[*]}"
    exit 0
fi

mkdir -p "$cache"

# Offline path. dpkg unpacks and configures a whole set in one call; if the
# cached set is incomplete (an older image, a newer dependency), the check
# below fails and the archive path takes over.
if compgen -G "$cache/*.deb" >/dev/null; then
    echo "apt: installing from cache: ${packages[*]}"
    if sudo dpkg -i "$cache"/*.deb >/dev/null 2>&1 && all_installed; then
        echo "apt: cache hit"
        exit 0
    fi
    echo "apt: cached set is incomplete, going to the archive"
fi

# Miss. Keep the .debs this time (docker-clean would delete them), and keep the
# Google source and the Azure mirror from failing the index update. Both
# guards end in `|| true`: under pipefail an empty grep is a failure.
sudo rm -f /etc/apt/apt.conf.d/docker-clean
{ sudo grep -rl 'dl.google.com' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null \
    | xargs -r sudo rm -f; } || true
sudo sed -i 's/azure\.archive\.ubuntu\.com/archive.ubuntu.com/g' \
    /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null || true

apt_opts=(-o Acquire::Retries=2 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20)
for i in 1 2; do
    if sudo timeout 120 apt-get update "${apt_opts[@]}"; then
        break
    fi
    if [ "$i" -eq 2 ]; then
        echo "apt: update failed after $i attempts" >&2
        exit 1
    fi
    sleep 10
done

sudo timeout 240 apt-get install -y --download-only "${apt_opts[@]}" "${packages[@]}"
if compgen -G "/var/cache/apt/archives/*.deb" >/dev/null; then
    sudo cp /var/cache/apt/archives/*.deb "$cache"/
fi
sudo dpkg -i "$cache"/*.deb
all_installed || {
    echo "apt: ${packages[*]} missing after install" >&2
    exit 1
}
echo "apt: installed from the archive; the next run is offline"
