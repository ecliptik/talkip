#!/bin/bash
# release.sh: build, package and publish a tagged release on Forgejo and GitHub.
# Run in a clean checkout of the tag (e.g. git clone --branch v0.1.0 https://github.com/ecliptik/talkip).
#   FORGEJO_TOKEN  Forgejo token with repository write (skipped if unset)
#   GH_TOKEN       GitHub token with Contents write for the repo, used by gh (skipped if unset)
#   TOOLCHAIN      as for build.sh
# Usage: ./scripts/release.sh v0.1.0
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=${1:?usage: release.sh vX.Y.Z}
VER=${TAG#v}
FORGEJO_URL=${FORGEJO_URL:?set FORGEJO_URL to the Forgejo base URL}
FORGEJO_REPO=${FORGEJO_REPO:-ecliptik/talkip}
GITHUB_REPO=${GITHUB_REPO:-ecliptik/talkip}

[ "$(git describe --tags --exact-match 2>/dev/null)" = "$TAG" ] || { echo "HEAD is not at tag $TAG" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "checkout is not clean" >&2; exit 1; }
grep -q "^project(talkip VERSION $VER " CMakeLists.txt || { echo "CMakeLists.txt is not version $VER" >&2; exit 1; }

./scripts/build.sh --clean >/dev/null
./scripts/package.sh
FILES=(build/talkip-$VER.dsk build/talkip-$VER.sit)

NOTES=$(sed -n "/^## \[$VER\]/,/^## \[/{/^## \[/d;p}" CHANGELOG.md)
NOTES+="
**Downloads:** \`talkip-$VER.dsk\` is an 800K disk image (for a Floppy Emu, BlueSCSI or an emulator), and \`talkip-$VER.sit\` is a StuffIt 1.5 archive of the same files. Each has the five TalkIP tools, the TalkIP Driver and AppleTalk On INITs, and a Read Me."
NAME="talkip $TAG"

if [ -n "${FORGEJO_TOKEN:-}" ]; then
    API=$FORGEJO_URL/api/v1/repos/$FORGEJO_REPO
    id=$(curl -sf -X POST "$API/releases" -H "Authorization: token $FORGEJO_TOKEN" -H "Content-Type: application/json" \
        -d "$(jq -n --arg t "$TAG" --arg n "$NAME" --arg b "$NOTES" '{tag_name:$t,name:$n,body:$b}')" | jq -r .id)
    for f in "${FILES[@]}"; do
        curl -sf -X POST "$API/releases/$id/assets?name=$(basename "$f")" -H "Authorization: token $FORGEJO_TOKEN" \
            -F "attachment=@$f" >/dev/null
    done
    echo "Forgejo: $FORGEJO_URL/$FORGEJO_REPO/releases/tag/$TAG"
else
    echo "FORGEJO_TOKEN not set: skipping Forgejo"
fi

if [ -n "${GH_TOKEN:-}" ]; then
    gh release create "$TAG" --repo "$GITHUB_REPO" --verify-tag --title "$NAME" --notes "$NOTES" "${FILES[@]}"
else
    echo "GH_TOKEN not set: skipping GitHub"
fi
