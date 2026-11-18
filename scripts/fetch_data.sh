#!/usr/bin/env bash
# Downloads the conformance suites into data/ (not committed), pinned to the
# commits the README results were measured on. Sparse checkouts keep it small.
set -e
cd "$(dirname "$0")/.."
mkdir -p data
want() { [ ${#SUITES[@]} -eq 0 ] || [[ " ${SUITES[*]} " == *" $1 "* ]]; }
SUITES=("$@")  # optional: a subset of html5lib-tests test262 wpt
fetch() { # dir url commit paths...
  local dir=$1 url=$2 rev=$3; shift 3
  want "$dir" || return 0
  if [ ! -d "data/$dir/.git" ]; then
    git clone --filter=blob:none --no-checkout "$url" "data/$dir"
  fi
  if [ $# -gt 0 ]; then git -C "data/$dir" sparse-checkout set --no-cone "$@"; fi
  git -C "data/$dir" fetch --depth 1 origin "$rev"
  git -C "data/$dir" checkout -q "$rev"
}
fetch html5lib-tests https://github.com/html5lib/html5lib-tests.git c5495c539ca8a79c3c5107f5f14010bcff9a35d5
fetch test262 https://github.com/tc39/test262.git 2e58f87d8350ad51224617a041c6e041d52f8444 \
  /harness/ /test/built-ins/Array/ /test/built-ins/Boolean/ /test/built-ins/Error/ /test/built-ins/Function/ \
  /test/built-ins/JSON/ /test/built-ins/Math/ /test/built-ins/Number/ /test/built-ins/Object/ /test/built-ins/String/ \
  /test/language/arguments-object/ /test/language/expressions/ /test/language/function-code/ /test/language/literals/ \
  /test/language/statements/ /test/language/types/
fetch wpt https://github.com/web-platform-tests/wpt.git edd042b63671f8ba064d86560836171e0cdb3956 \
  /css/CSS2/margin-padding-clear/ /css/CSS2/box-display/ /css/CSS2/normal-flow/ /css/CSS2/borders/ /css/CSS2/colors/ \
  /css/CSS2/backgrounds/ /css/css-flexbox/ /css/CSS2/support/ /css/support/ /css/reference/ /css/CSS2/reference/ \
  /fonts/ /html/syntax/parsing/resources/
if [ ${#SUITES[@]} -gt 0 ] && want wpt-parsing && ! want wpt; then  # only the tree-construction tests
  SUITES=(wpt)
  fetch wpt https://github.com/web-platform-tests/wpt.git edd042b63671f8ba064d86560836171e0cdb3956 /html/syntax/parsing/resources/
fi
echo "data ready in data/"
