{
  description = "tbd";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        myflut = pkgs.stdenv.mkDerivation {
          name = "myflut";
          pname = "myflut";
          buildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.cmake
            pkgs.glfw3
            pkgs.glew
            pkgs.libuv
          ];
          src = ./.;

          mesonBuildType = "release";
        };

        myflut-debug = pkgs.stdenv.mkDerivation {
          name = "myflut-debug";
          pname = "myflut-debug";
          buildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.cmake
            pkgs.glfw3
            pkgs.glew
            pkgs.libuv
          ];
          src = ./.;

          dontStrip = true;
          mesonBuildType = "debugoptimized";
        };

        sizeCommand = pkgs.writeShellApplication {
          name = "sizeCommand";
          runtimeInputs = [ pkgs.inetutils ];
          text = ''
          printf "SIZE\nSIZE\n" | nc 0.0.0.0 1337
          '';
        };

        redPixels = pkgs.writeShellApplication {
          name = "redPixels";
          runtimeInputs = [ pkgs.inetutils ];
          text = ''
          printf "1 1 ff0000\n2 1 ff0000\n3 1 ff0000\n" | nc 0.0.0.0 1337
          '';
        };
      in
      {
        devShells.default = with pkgs; pkgs.mkShell {
          stdenv = clangStdenv;
          nativeBuildInputs = [ clang-tools ];
          packages = [
            inetutils
            gdb
            sizeCommand
            redPixels
          ];
          shellHook = ''zsh'';
        };
        packages = {
          myflut = myflut;
          myflut-debug = myflut-debug;
          default = myflut;
        };
      }
    );
}
