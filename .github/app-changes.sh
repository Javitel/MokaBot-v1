#!/bin/bash
# Which apps changed since the previous release tag (used by release.yml and
# testflight.yml). Writes to $GITHUB_OUTPUT:
#   previous=<tag>   the release before this commit ("" when there is none)
#   android=true|false, ios=true|false
# An app counts as changed when any of its sources differ, including the web
# pages and bridge it bundles; with no previous release both are built.
set -euo pipefail
out="${GITHUB_OUTPUT:-/dev/stdout}"

# The newest earlier release tag that has a published release (a tag whose
# release failed, e.g. v1.0.9, has no APK / IPA to reuse). Without the gh
# CLI or a token (run locally), the newest earlier tag.
previous=""
for tag in $(git tag --merged HEAD^ --list 'v*' --sort=-v:refname); do
  if [ -z "${GH_TOKEN:-}" ] || gh release view "$tag" >/dev/null 2>&1; then
    previous=$tag
    break
  fi
done
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
