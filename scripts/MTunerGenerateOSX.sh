#!/bin/bash
target="$1"

if [ -z "$target" ]; then
action="--gcc=osx-x64 gmake"
fi

cd ..
zidar/tools/bin/darwin/genie --file=src/MTuner/scripts/genie.lua $action
cd scripts

