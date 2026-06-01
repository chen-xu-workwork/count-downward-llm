# shell.nix

{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = [
    #pkgs.cmake
    #pkgs.stdenv
    # pkgs.cplex
    pkgs.python3
    pkgs.bear
    pkgs.gnumake
    pkgs.gcc
    #pkgs.osi
  ];
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.gcc
    pkgs.gcc_multi
    pkgs.bear
    pkgs.gnumake
    pkgs.python3.pkgs.wrapPython
    pkgs.gcc
  ];
}
