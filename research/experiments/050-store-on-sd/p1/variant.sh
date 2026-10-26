#!/bin/sh
# variant.sh NAME COMMIT [PATCH...]: pure-serve and pure-crypto.so of COMMIT with PATCHes applied, in ~/.cache/pure-040/NAME
# (as 040's runs.sh serve does, in a scratch worktree). A PATCH named nocompact raises the compaction trigger instead.
set -e
name=$1 commit=$2; shift 2
builds=$HOME/.cache/pure-040
rm -rf "$builds/src-$name"
git worktree add -f -q --detach "$builds/src-$name" "$commit"
for patch in "$@"; do
    if [ "$patch" = nocompact ]; then
        sed -i 's/enum { COMPACT_SLACK = 1048576 };/enum { COMPACT_SLACK = 1 << 30 }; \/* variant: no compaction in a run *\//' "$builds/src-$name/store/store-tree.c"
        grep -q 'variant: no compaction' "$builds/src-$name/store/store-tree.c"
    else
        git -C "$builds/src-$name" apply "$patch"
    fi
done
make -s -C "$builds/src-$name" build/pure-serve build/pure-crypto.so
mkdir -p "$builds/$name"
cp "$builds/src-$name/build/pure-serve" "$builds/src-$name/build/pure-crypto.so" "$builds/$name/"
git -C "$builds/src-$name" diff --stat > "$builds/$name/variant.txt"
echo "$commit $*" >> "$builds/$name/variant.txt"
git worktree remove --force "$builds/src-$name"
