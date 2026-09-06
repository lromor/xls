{
  # Nix flake for XLS: a development shell and a pre-built `xls` package.
  #
  # Development shell (bazel and the host tools the build reaches for):
  #
  #   nix develop            # or `direnv allow`
  #   bazel build //xls/dslx/ir_convert:ir_converter_main
  #
  # Consuming the pre-built tools from another flake:
  #
  #   inputs.xls.url = "github:lromor/xls/nix";
  #   ...
  #   inputs.xls.packages.${system}.default    # or overlays.default -> pkgs.xls
  #
  # and make the binary cache known to the consuming side, either in its own
  # flake's nixConfig or in nix.conf:
  #
  #   extra-substituters = https://fpga-assembler.cachix.org
  #   extra-trusted-public-keys = fpga-assembler.cachix.org-1:yp4kNzY1Hru9BlaoE025RuBaL/sX6Ou2abo5L8SHk0I=
  #
  # Building the package yourself: Bazel needs the network during the build
  # (bzlmod registry, git_override() clones) and the prebuilt Python it
  # downloads needs the host's nix-ld loader. Neither is available inside the
  # nix sandbox, so the derivation opts out of it with __noChroot. Build it as
  # a trusted user on a NixOS host with programs.nix-ld enabled:
  #
  #   nix build --option sandbox relaxed .#xls
  #   cachix push fpga-assembler ./result
  #
  # Consumers never build it, they fetch it from the cache.
  # This is not officially supported by the XLS team.
  description = "XLS: Accelerated HW Synthesis";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  nixConfig = {
    extra-substituters = [
      "https://fpga-assembler.cachix.org"
    ];
    extra-trusted-public-keys = [
      "fpga-assembler.cachix.org-1:yp4kNzY1Hru9BlaoE025RuBaL/sX6Ou2abo5L8SHk0I="
    ];
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Bazel targets that make up the package.
      bazelTargets = [
        "//xls/dslx/ir_convert:ir_converter_main"
        "//xls/dslx:interpreter_main"
        "//xls/dslx:dslx_fmt"
        "//xls/dslx/lsp:dslx_ls"
        "//xls/tools:opt_main"
        "//xls/tools:codegen_main"
        "//xls/tools:eval_ir_main"
      ];

      # Short names, matching the XLS_* tool variables in our Makefiles.
      aliases = {
        xls-ir-converter = "ir_converter_main";
        xls-interpreter = "interpreter_main";
        xls-opt = "opt_main";
        xls-codegen = "codegen_main";
      };

      mkXls =
        pkgs:
        let
          inherit (pkgs) lib;
          # "//xls/tools:opt_main" -> "xls/tools/opt_main" (path under bazel-bin).
          targetPath = t: builtins.replaceStrings [ "//" ":" ] [ "" "/" ] t;
        in
        pkgs.stdenv.mkDerivation {
          pname = "xls";
          version = "0-unstable-2026-09-06";
          src = self;

          # See the header comment: network + nix-ld are needed at build time.
          __noChroot = true;
          preferLocalBuild = true;

          # stdenv also puts gcc/g++/ar on PATH, which Z3's mk_make.py source
          # generator probes for before it emits anything. The C++ compilation
          # itself uses the hermetic LLVM toolchain that bzlmod downloads.
          nativeBuildInputs = with pkgs; [
            bazel_8
            jdk
            git
            cacert
            python3
            autoPatchelfHook
          ];

          env = {
            SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
            GIT_SSL_CAINFO = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
            BAZEL_SH = "${pkgs.bash}/bin/bash";
            # For the prebuilt binaries bazel downloads (python, pip wheels).
            NIX_LD = "${pkgs.stdenv.cc.bintools.dynamicLinker}";
            NIX_LD_LIBRARY_PATH = lib.makeLibraryPath [
              pkgs.stdenv.cc.cc.lib
              pkgs.zlib
            ];
          };

          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            export HOME="$TMPDIR/home"
            mkdir -p "$HOME"
            bazel --output_user_root="$TMPDIR/bazel" build \
              -c opt \
              --spawn_strategy=local \
              --curses=no --color=no --show_progress_rate_limit=60 \
              --jobs="$NIX_BUILD_CORES" \
              --verbose_failures \
              ${lib.escapeShellArgs bazelTargets}
            runHook postBuild
          '';

          postBuild = ''
            bazel --output_user_root="$TMPDIR/bazel" shutdown || true
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out/bin" "$out/libexec/xls"
            for t in ${lib.escapeShellArgs (map targetPath bazelTargets)}; do
              name="$(basename "$t")"
              install -Dm755 "bazel-bin/$t" "$out/libexec/xls/$name"
              # The runfiles tree (DSLX stdlib) must sit next to the binary; the
              # tools locate it through the real path of their executable.
              if [ -d "bazel-bin/$t.runfiles" ]; then
                cp -rL "bazel-bin/$t.runfiles" "$out/libexec/xls/$name.runfiles"
                rm -f "$out/libexec/xls/$name.runfiles/MANIFEST"
                self_copy="$out/libexec/xls/$name.runfiles/_main/$t"
                if [ -e "$self_copy" ]; then
                  rm -f "$self_copy"
                  ln -s "$(realpath --relative-to="$(dirname "$self_copy")" "$out/libexec/xls/$name")" "$self_copy"
                fi
              fi
              ln -s "../libexec/xls/$name" "$out/bin/$name"
            done
            ${lib.concatStringsSep "\n" (
              lib.mapAttrsToList (alias: target: ''ln -s "${target}" "$out/bin/${alias}"'') aliases
            )}
            runHook postInstall
          '';

          meta = with lib; {
            description = "XLS: Accelerated HW Synthesis (DSLX front end, IR optimizer, codegen)";
            homepage = "https://github.com/google/xls";
            license = licenses.asl20;
            platforms = [ "x86_64-linux" ];
            mainProgram = "interpreter_main";
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

      devShells = forAllSystems (pkgs: {
        # mkShell puts stdenv's gcc/g++/ar on PATH; keep it that way, Z3's
        # mk_make.py needs them (see mkXls above).
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
          ];

          CLANG_TIDY = "${pkgs.clang-tools}/bin/clang-tidy";
          CLANG_FORMAT = "${pkgs.clang-tools}/bin/clang-format";
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
