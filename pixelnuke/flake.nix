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
      in
      {
        packages = {
          myflut = myflut;
          myflut-debug = myflut-debug;
          default = myflut;
        };
      }
    );
}
