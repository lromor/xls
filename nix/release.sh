#!/usr/bin/env bash
# Assemble the prebuilt binary tarball that flake.nix packages.
#
# Usage, from the repository root:
#
#   nix develop
#   bazel build -c opt $(nix/release.sh --targets)
#   nix/release.sh [version]
#
# Stages the tools, the DSLX stdlib and LICENSE in the same layout as the
# official google/xls release tarballs, writes xls-<version>-linux-x64.tar.gz
# to the current directory and prints the SRI hash flake.nix needs. The
# version defaults to upstream's scheme (commits since the v0.0.0 tag).
# BAZEL_BIN can point at the bazel-bin of a different checkout.
set -euo pipefail

targets=(
  //xls/dslx/ir_convert:ir_converter_main
  //xls/dslx:interpreter_main
  //xls/dslx:dslx_fmt
  //xls/dslx/lsp:dslx_ls
  //xls/dslx:parse_and_typecheck_dslx_main
  //xls/dslx:prove_quickcheck_main
  //xls/tools:opt_main
  //xls/tools:codegen_main
  //xls/tools:proto_to_dslx_main
  //xls/tools:eval_ir_main
)

if [[ "${1:-}" == "--targets" ]]; then
  printf '%s\n' "${targets[@]}"
  exit 0
fi

cd "$(dirname "$0")/.."
bazel_bin=${BAZEL_BIN:-bazel-bin}
version=${1:-$(git describe --tags --match=v0.0.0 HEAD)}
name="xls-${version}-linux-x64"

staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
mkdir -p "$staging/$name/xls/dslx/stdlib"
for t in "${targets[@]}"; do
  p=${t#//}
  p=${p/:/\/}
  install -m755 "$bazel_bin/$p" "$staging/$name/"
done
cp xls/dslx/stdlib/*.x "$staging/$name/xls/dslx/stdlib/"
cp LICENSE "$staging/$name/"

# Deterministic archive: same inputs give the same bytes and hash.
tar -C "$staging" --sort=name --owner=0 --group=0 --numeric-owner \
  --mtime='1970-01-01 00:00:00Z' -cf - "$name" | gzip -n > "$name.tar.gz"

echo "wrote:   $name.tar.gz"
echo "version: $version"
echo "hash:    $(nix hash file --sri --type sha256 "$name.tar.gz")"
