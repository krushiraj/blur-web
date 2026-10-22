#!/bin/bash
# usage: conv.sh <file.spv>   (run from a work dir containing wgsl/ err/ opt/; set NAGA to the naga binary, default: naga on PATH)
# naga SPIR-V -> WGSL; on failure retry after spirv-opt constant folding (fixes OpSNegate on u32 constants)
NAGA=${NAGA:-naga}; f=$1; b=$(basename $f .spv); mkdir -p wgsl err opt
if $NAGA --keep-coordinate-space $f wgsl/$b.wgsl > err/$b.txt 2>&1; then rm err/$b.txt; else
 spirv-opt --ccp --simplify-instructions --eliminate-dead-code-aggressive $f -o opt/$b.spv && $NAGA --keep-coordinate-space opt/$b.spv wgsl/$b.wgsl > err/$b.txt 2>&1 && echo "retry-ok $b" >> retry.log && rm err/$b.txt; fi
