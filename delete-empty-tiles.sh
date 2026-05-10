#!/bin/bash

find "$1" -type f -iname '*.png' -exec sh -c '
for f do
    if identify -format "%[opaque]" "$f" 2>/dev/null | grep -q false; then
        echo "Deleting empty tile: $f"
        rm "$f"
    fi
done
' sh {} +