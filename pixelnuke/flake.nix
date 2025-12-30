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
          printf "SIZE" | nc 0.0.0.0 1337
          '';
        };

        redPixels = pkgs.writeShellApplication {
          name = "redPixels";
          runtimeInputs = [ pkgs.inetutils ];
          text = ''
          printf "PX 1 1 ff0000\nPX 2 1 ff0000\nPX 3 1 ff0000\n" | nc 0.0.0.0 1337
          '';
        };

        manyPixels = pkgs.writeShellApplication {
          name = "manyPixels";
          runtimeInputs = [ pkgs.inetutils ];
          text = ''
	  for column in $(seq 1 1);
	  do
	    A="PX 0 0 ff0000";
	    for row in $(seq 1 500);
	    do
	        A="$A\nPX $column $row ff0000";
	    done
	    echo "$A" | nc 0.0.0.0 1337
	  done
          '';
        };

        resetPixels = pkgs.writeShellApplication {
          name = "resetPixels";
          runtimeInputs = [ pkgs.inetutils ];
          text = ''
	  printf "RESET" | nc 0.0.0.0 1337
          '';
        };
      in
      {
        devShells.default = with pkgs; mkShell {
          stdenv = clangStdenv;
          nativeBuildInputs = [ clang-tools ];
          packages = [
            inetutils
            gdb
            sizeCommand
            redPixels
	    manyPixels
	    resetPixels
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
