#!/bin/bash
# The GPU layer's structure rule (docs/hle-gpu-layers.md): nothing but the
# fixes layer asks which title is running, whether it links Direct3D, or which
# build of it. The base is the NV2A and is the same for every title; what a
# title needs on top of it is a row of hw/xbox/hle/fixes/fixes.c, tested with
# hle_fix().
#
#   tools/native/check-layers.sh
#
# Exits 1 and prints each offending line when the rule is broken. Names in
# comments count too: a comment that says a rule is for "a title without D3D"
# is how a rule gets gated again.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HLE="$REPO/hw/xbox/hle"
cd "$HLE" || exit 1

# the title's identity, however it is asked
CODE='hle_xbe_d3d_lib|hle_xbe_find_lib|g_bare_title|g_pd\.bare|hle_pbd_bare|\.bare\b|title_id *[=!<>]=|title_id *&|lib->build|lib->major|libs\[[a-z0-9]*\]\.build'
# the words a gated rule leaves in a comment
PROSE='(a )?titles? without D3D|titles? with D3D|titles? that (has|have) no D3D|homebrew|bare title'

# the fixes layer itself and the XBE parser say who the title is; the rest
# of core/ parses and keeps the identity for diagnostics only
ALLOW='^(fixes/|core/xbe\.[ch]$)'
bad=0
while IFS= read -r f; do
  f=${f#./}
  [[ $f =~ $ALLOW ]] && continue
  if out=$(grep -nE "$CODE" "$f"); then
    echo "$f: asks which title runs (a fix belongs in fixes/fixes.c):"
    echo "$out" | sed 's/^/    /'
    bad=1
  fi
  if out=$(grep -nEi "$PROSE" "$f"); then
    echo "$f: a rule described as for some titles (the base is for all; a title's difference is a fix row):"
    echo "$out" | sed 's/^/    /'
    bad=1
  fi
done < <(find . -name '*.c' -o -name '*.h' | sort)
[ $bad = 0 ] && echo "hle layers: the base asks no title's identity"
exit $bad
