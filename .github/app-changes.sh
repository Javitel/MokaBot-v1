#!/bin/bash
# Which apps changed since the previous release tag (used by release.yml and
# testflight.yml). Writes to $GITHUB_OUTPUT:
#   previous=<tag>   the release before this commit ("" when there is none)
#   android=true|false, ios=true|false
# An app counts as changed when any of its sources differ, including the web
# pages and bridge it bundles; with no previous release both are built.
set -euo pipefail
out="${GITHUB_OUTPUT:-/dev/stdout}"

previous=$(git describe --tags --abbrev=0 --match 'v*' HEAD^ 2>/dev/null || true)
echo "previous=$previous" >> "$out"

shared=(mobile data/brew.html data/settings.html)
changed() {
  [ -z "$previous" ] && { echo true; return; }
  if git diff --quiet "$previous" HEAD -- "$@" "${shared[@]}"; then echo false; else echo true; fi
}
android=$(changed android)
ios=$(changed ios)
echo "android=$android" >> "$out"
echo "ios=$ios" >> "$out"
echo "Since ${previous:-the start}: Android app changed: $android, iOS app changed: $ios" >&2
