{
  # Nix flake for XLS: a development shell and a package of prebuilt tools.
  #
  # Development shell (bazel plus the host tools the build reaches for):
  #
  #   nix develop            # or `direnv allow`
  #   bazel build //xls/dslx/ir_convert:ir_converter_main
  #
  # Using the prebuilt tools from another flake:
  #
  #   inputs.xls.url = "github:lromor/xls/nix";
  #   ...
  #   inputs.xls.packages.${system}.default    # or overlays.default -> pkgs.xls
  #
  # The package downloads a tarball attached to a GitHub release on this
  # fork, so consumers need nothing else configured. Layout of the result:
  #
  #   bin/xls-ir-converter, bin/xls-interpreter, bin/xls-opt, bin/xls-codegen,
  #   bin/xls-parse-and-typecheck-dslx, bin/xls-prove-quickcheck,
  #   bin/xls-proto-to-dslx, bin/xls-eval-ir, bin/dslx-fmt, bin/dslx-ls
  #   libexec/xls/<bazel names>      the real binaries
  #   lib/xls/dslx/stdlib            the DSLX standard library
  #
  # The tools find the stdlib on their own; --dslx_stdlib_path is optional.
  #
  # Publishing new binaries is manual (no CI), from a checkout of this branch:
  #
  #   nix develop
  #   bazel build -c opt $(nix/release.sh --targets)
  #   nix/release.sh                        # tarball + version + hash
  #   gh release create nix-<version> xls-<version>-linux-x64.tar.gz \
  #     --repo lromor/xls --target <commit>
  #
  # then update `release` below, commit and push.
  # This is not officially supported by the XLS team.
  description = "XLS: Accelerated HW Synthesis";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The prebuilt binaries: output of nix/release.sh, attached to the GitHub
      # release `nix-<version>` on lromor/xls.
      release = {
        version = "v0.0.0-10627-gc42eadd12";
        hash = "sha256-cootrMpAdBT6Wc+vJ+Ci++AMr2vW5hPfLWMkFlvxhbM=";
      };

      mkXls =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "xls";
          inherit (release) version;

          src = pkgs.fetchurl {
            url = "https://github.com/lromor/xls/releases/download/nix-${release.version}/xls-${release.version}-linux-x64.tar.gz";
            inherit (release) hash;
          };

          nativeBuildInputs = [ pkgs.autoPatchelfHook ];

          dontConfigure = true;
          dontBuild = true;

          installPhase = ''
            runHook preInstall
            mkdir -p "$out/bin" "$out/libexec/xls" "$out/lib/xls/dslx"
            cp -r xls/dslx/stdlib "$out/lib/xls/dslx/"

            for f in *_main dslx_fmt dslx_ls; do
              install -m755 "$f" "$out/libexec/xls/$f"

              # A runfiles tree next to each binary lets the tools locate the
              # DSLX stdlib without flags, the same way they do under bazel.
              runfiles="$out/libexec/xls/$f.runfiles"
              mkdir -p "$runfiles/_main/xls/dslx"
              ln -s "$out/lib/xls/dslx/stdlib" "$runfiles/_main/xls/dslx/stdlib"
              printf ',com_google_xls,_main\n' > "$runfiles/_repo_mapping"

              # Friendlier names in bin: xls- prefix, no _main suffix, dashes.
              case "$f" in
                *_main) nice="xls-$(echo "''${f%_main}" | tr _ -)" ;;
                *) nice="$(echo "$f" | tr _ -)" ;;
              esac
              ln -s "../libexec/xls/$f" "$out/bin/$nice"
            done
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "XLS: Accelerated HW Synthesis (DSLX front end, IR optimizer, codegen)";
            homepage = "https://github.com/google/xls";
            license = licenses.asl20;
            platforms = [ "x86_64-linux" ];
            mainProgram = "xls-interpreter";
          };
        };
    in
    {
      packages = forAllSystems (pkgs: rec {
        xls = mkXls pkgs;
        default = xls;
      });

      overlays.default = final: prev: {
        xls = self.packages.${final.stdenv.hostPlatform.system}.xls;
      };

      devShells = forAllSystems (
        pkgs:
        let
          # PATH that bazel build actions see. It is part of every action's
          # cache key, so it must not change when the dev shell changes:
          # pin it to a fixed set of tools instead of inheriting the shell's
          # PATH (bazel's default). The C++ compilation uses the hermetic LLVM
          # toolchain bzlmod downloads; gcc is here because Z3's mk_make.py
          # source generator probes PATH for a C++ compiler and `ar`.
          actionPath = pkgs.lib.makeBinPath (
            pkgs.stdenv.initialPath
            ++ [
              pkgs.stdenv.cc
              pkgs.stdenv.cc.bintools
              pkgs.bash
              pkgs.python3
              pkgs.git
              pkgs.perl
            ]
          );
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              bazel_8 # 8.7.0, see .bazelversion
              jdk
              git
              cacert # git_override() modules are cloned over https.
              python3
              perl # iverilog (via rules_hdl) uses perl to create its config.h
              ncurses # provides tic
              zlib

              # Development support.
              bazel-buildtools # buildifier, buildozer
              clang-tools # clang-format, clang-tidy
              gh # publishing releases, see nix/release.sh
            ];

            CLANG_TIDY = "${pkgs.clang-tools}/bin/clang-tidy";
            CLANG_FORMAT = "${pkgs.clang-tools}/bin/clang-format";

            # .bazelrc try-imports user.bazelrc (gitignored); own it only when
            # it is ours, so a hand-written one is never clobbered.
            shellHook = ''
              root=$(git rev-parse --show-toplevel 2>/dev/null || echo "$PWD")
              rc="$root/user.bazelrc"
              marker="# Generated by the nix dev shell (flake.nix); do not edit."
              if [ ! -e "$rc" ] || grep -qF "$marker" "$rc"; then
                printf '%s\n%s\n' "$marker" \
                  "build --action_env=PATH=${actionPath}" > "$rc"
              else
                echo "flake.nix: not touching existing $rc; add to it:" >&2
                echo "  build --action_env=PATH=${actionPath}" >&2
              fi
            '';
          };
        }
      );

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
