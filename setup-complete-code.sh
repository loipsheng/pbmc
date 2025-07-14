#!/bin/bash

# Exit on any error
# set -e

# Create and enter the directory
mkdir -p temp-source-clones
cd temp-source-clones

# Define repositories and commits
GPMC_URL="https://git.trs.css.i.nagoya-u.ac.jp/k-hasimt/GPMC.git"
GPMC_COMMIT="6eea7d6a39edbfd530f2db2cafe1993b924ea298"

RS_URL="https://gitlab.com/MIAOresearch/software/roundingsat.git"
RS_COMMIT="c548e1098a81d1f57dfc31560208034253d174c1"

# Clone GPMC repository and checkout to specific commit
git clone "$GPMC_URL"
cd "$(basename "$GPMC_URL" .git)"
git checkout "$GPMC_COMMIT"
cd ..

# Clone RoundingSat repository and checkout to specific commit
git clone "$RS_URL"
cd "$(basename "$RS_URL" .git)"
git checkout "$RS_COMMIT"
cd ..

echo "Repositories cloned and checked out to specified commits."
cd ..

# Copying missing files over from cloned repositories
# cp -nr ./temp-source-clones/roundingsat/src/used_licenses ./roundingsat/
mkdir -p ./roundingsat
cp -r ./roundingsat-mod/. ./roundingsat/
cp -nr ./temp-source-clones/roundingsat/src/. ./roundingsat/

echo "Completed roundingsat files"

# flowcutter, ddnnf, mpfr, mtl, preprocessor, utils, core
cp -r ./pbmc-mod/. ./
mkdir -p flow-cutter-pace17 ddnnf mpfr mtl preprocessor utils core
cp -nr ./temp-source-clones/GPMC/flow-cutter-pace17/* ./flow-cutter-pace17/
cp -nr ./temp-source-clones/GPMC/ddnnf/* ./ddnnf/
cp -nr ./temp-source-clones/GPMC/mpfr/* ./mpfr/
cp -nr ./temp-source-clones/GPMC/mtl/* ./mtl/
cp -nr ./temp-source-clones/GPMC/preprocessor/* ./preprocessor/
cp -nr ./temp-source-clones/GPMC/utils/* ./utils/
cp -nr ./temp-source-clones/GPMC/core/* ./core/

echo "Completed GPMC files"
# Removing temporary source cloning folder
rm -rf ./temp-source-clones

echo "Completed downloading necessary source files"
echo "Please compile using ./build.sh r (non static -- mac) or ./build.sh rs (static -- linux)"