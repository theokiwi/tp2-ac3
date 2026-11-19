#!/usr/bin/env bash
# Wrapper to run a python command with matplotlib/numpy/pandas available via nix.
exec nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (ps: [ ps.matplotlib ps.numpy ps.pandas ])' --command python3 "$@"
