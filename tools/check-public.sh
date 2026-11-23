#!/bin/sh
# The repo is public. Run before every push: checks what would go out
# (commits not yet on origin/main, plus staged changes) for credentials,
# keys, addresses, device serials and files that don't belong. Exit 1 on
# anything found, or if it can't tell.
#   tools/check-public.sh            unpushed commits + staged changes
#   tools/check-public.sh --all      the whole history
set -eu
cd "$(dirname "$0")/.."
if [ "${1:-}" = "--all" ]; then
  RANGE="--all"
else
  git fetch -q origin main 2>/dev/null || true
  git rev-parse -q --verify origin/main >/dev/null || { echo "no origin/main: can't tell what is unpushed"; exit 1; }
  RANGE="origin/main..HEAD"
fi
bad=0
added=$( { git log $RANGE -p -- . ':!lib' ':!tools/check-public.sh'; [ "$RANGE" = "--all" ] || git diff --cached -- . ':!lib' ':!tools/check-public.sh'; } | grep -E '^\+' || true)
hits=$(printf '%s\n' "$added" | grep -inE \
  'psk[=:][^ ]|BEGIN [A-Z ]*PRIVATE KEY|ghp_[A-Za-z0-9]{20}|github_pat_|sk-ant-|xox[bpa]-|AKIA[0-9A-Z]{16}|([0-9a-f]{2}:){5}[0-9a-f]{2}|192\.168\.[0-9]+\.[0-9]+|(^|[^0-9.])10\.[0-9]+\.[0-9]+\.[0-9]+|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]+\.[0-9]+|serial[" :=]+[0-9A-F]{16,}|password *[=:] *["'"'"'][^"'"'"']+' \
  | grep -vE 'noreply@|10\.3\.1\.1\.|10\.42\.0\.(1|10|99)([^0-9]|$)' || true)
if [ -n "$hits" ]; then echo "possible secrets or addresses:"; printf '%s\n' "$hits" | head -20; bad=1; fi
files=$( { git log $RANGE --name-only --format= -- . ':!lib' ':!tools/check-public.sh'; [ "$RANGE" = "--all" ] || git diff --cached --name-only -- . ':!lib' ':!tools/check-public.sh'; } | sort -u | grep -iE \
  '\.(pem|key|crt|p12|pfx|img|iso|dump|raw|env|wav|aiff?|mp3|flac|ogg|webm|m4a|heic|jpe?g|mov)$|state\.json|wpa_supplicant|netplan|id_(rsa|ed25519)' || true)
if [ -n "$files" ]; then echo "files that shouldn't be public:"; printf '%s\n' "$files"; bad=1; fi
[ $bad -eq 0 ] && echo "clean: nothing private in ${RANGE} (and staged changes)"
exit $bad
