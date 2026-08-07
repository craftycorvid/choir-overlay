#!/usr/bin/env bash
#
# Publish Choir's AUR packages from the tracked PKGBUILDs in this directory.
#
#   bash packaging/aur/publish.sh [git|stable|both] [commit message]
#
#   git     -> choir-overlay-git  (builds the latest commit on main)
#   stable  -> choir-overlay      (builds the tagged release tarball)
#   both    -> both of the above  (default)
#
# Each package is published by cloning its AUR repo into a FRESH TEMP DIR, copying
# PKGBUILD + .SRCINFO over it, committing and pushing. The script keeps no state between
# runs, so it never depends on a leftover checkout and is safe to re-run after a failure.
# A package whose AUR repo doesn't exist yet is created by the push (initial import).
#
# This only publishes. Build first — `cd packaging/aur/<dir> && makepkg -f` — since an
# AUR package that doesn't compile is everyone's problem.
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Overridable so the script can be exercised against local bare repos standing in for the
# AUR (AUR_GIT_BASE="file:///tmp/fakeaur"); defaults to the real thing.
AUR_GIT_BASE="${AUR_GIT_BASE:-ssh://aur@aur.archlinux.org}"

TARGET="${1:-both}"
MESSAGE="${2:-}"

case "${TARGET}" in
  git)    DIRS=(git) ;;
  stable) DIRS=(stable) ;;
  both)   DIRS=(git stable) ;;
  -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
  *) echo "publish.sh: unknown target '${TARGET}' (want: git, stable, both)" >&2; exit 2 ;;
esac

# ---- fail fast on AUR maintenance ------------------------------------------------------
# Otherwise `git clone` just fails and we'd fall through to the initial-import path, which
# then dies at push with a much less obvious error.
if [ "${AUR_GIT_BASE}" = "ssh://aur@aur.archlinux.org" ]; then
  status="$(ssh -o ConnectTimeout=10 aur@aur.archlinux.org help 2>&1 || true)"
  if printf '%s' "${status}" | grep -qi "maintenance\|down"; then
    echo "publish.sh: the AUR is not accepting pushes right now:" >&2
    printf '  %s\n' "${status}" >&2
    exit 1
  fi
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# Read one `key=value` line out of a PKGBUILD. Parsed rather than sourced: a PKGBUILD is a
# shell script, and sourcing it would run whatever is in it just to read a version string.
pkgfield() {  # pkgfield <pkgbuild> <key>
  grep -m1 "^$2=" "$1" | cut -d= -f2- | tr -d "'\""
}

publish_one() {  # publish_one <dir>
  local dir="$1" src="${SCRIPT_DIR}/$1"
  [ -f "${src}/PKGBUILD" ] || { echo "publish.sh: no PKGBUILD in ${src}" >&2; return 1; }

  local pkgname pkgver pkgrel
  pkgname="$(pkgfield "${src}/PKGBUILD" pkgname)"
  pkgver="$(pkgfield "${src}/PKGBUILD" pkgver)"
  pkgrel="$(pkgfield "${src}/PKGBUILD" pkgrel)"
  [ -n "${pkgname}" ] || { echo "publish.sh: no pkgname in ${src}/PKGBUILD" >&2; return 1; }

  # The AUR rejects a .SRCINFO that disagrees with its PKGBUILD, and a stale one silently
  # misreports depends to users (this bit us: the live package was missing libglvnd for
  # months). Regenerate and compare rather than trusting the committed copy.
  ( cd "${src}" && makepkg --printsrcinfo ) > "${TMP}/${dir}.srcinfo"
  if ! diff -q "${src}/.SRCINFO" "${TMP}/${dir}.srcinfo" >/dev/null; then
    echo "publish.sh: ${src}/.SRCINFO is stale. Regenerate it:" >&2
    echo "  (cd ${src} && makepkg --printsrcinfo > .SRCINFO)" >&2
    diff -u "${src}/.SRCINFO" "${TMP}/${dir}.srcinfo" >&2 || true
    return 1
  fi

  local url="${AUR_GIT_BASE}/${pkgname}.git" work="${TMP}/${pkgname}"
  # The AUR hands back an EMPTY repo when the package doesn't exist yet — that is the
  # documented initial-import flow, and the push is what actually creates the package.
  if git clone -q "${url}" "${work}" 2>/dev/null && [ -d "${work}/.git" ]; then
    :
  else
    rm -rf "${work}"
    git init -q "${work}"
    git -C "${work}" remote add origin "${url}"
  fi

  # An empty clone (or a fresh init) puts HEAD on whatever init.defaultBranch says, which
  # is 'main' on many setups — but the AUR only takes 'master'. Point HEAD at master while
  # it is still unborn; a populated clone is already on master and is left alone.
  if ! git -C "${work}" rev-parse --verify -q HEAD >/dev/null; then
    git -C "${work}" symbolic-ref HEAD refs/heads/master
  fi

  cp "${src}/PKGBUILD" "${src}/.SRCINFO" "${work}/"
  git -C "${work}" add -A
  if git -C "${work}" diff --cached --quiet; then
    echo ">> ${pkgname}: already up to date (${pkgver}-${pkgrel}), nothing to push"
    return 0
  fi

  git -C "${work}" commit -q -m "${MESSAGE:-${pkgname} ${pkgver}-${pkgrel}}"
  git -C "${work}" push -q origin master
  echo ">> ${pkgname}: pushed ${pkgver}-${pkgrel}"
}

for d in "${DIRS[@]}"; do
  publish_one "${d}"
done
