let
  pkgs = import <nixpkgs> { overlays = [ ]; };
in
pkgs.mkShell rec {
  buildInputs = with pkgs; [
    nixfmt-classic
    glew
    glfw
    libevent
    libGLU
    libuv
    meson
  ];
}
