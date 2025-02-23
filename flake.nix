{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix.url = "github:numtide/treefmt-nix";
    treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    inputs@{
      nixpkgs,
      parts,
      treefmt-nix,
      ...
    }:
    parts.lib.mkFlake { inherit inputs; } {
      imports = [
        treefmt-nix.flakeModule
      ];
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      perSystem =
        {
          config,
          pkgs,
          system,
          ...
        }:
        let
          riscvPkgs = import nixpkgs {
            localSystem = "${system}";
            crossSystem = {
              config = "riscv64-unknown-linux-gnu";
              gcc.abi = "ilp32";
            };
          };
          riscvBuildPackages = riscvPkgs.buildPackages;
          llvmName = "llvmPackages_20";
          deps = with pkgs; [
            git
            gnumake
          ];
        in
        {
          treefmt = {
            projectRootFile = "flake.nix";
            programs = {
              nixfmt.enable = true;
              nixfmt.package = pkgs.nixfmt-rfc-style;
            };
            flakeCheck = true;
          };
          packages = {
            default = config.packages.hello;
            hello = pkgs.callPackage ./nix/pkgs/tcc.nix { };
          };
          devShells.default =
            with pkgs;
            mkShell {
              nativeBuildInputs = [
                cmake
                gnumake
                riscvBuildPackages.glibc
              ] ++ lib.optional stdenv.hostPlatform.isLinux riscvPkgs.buildPackages.gdb;
              buildInputs = [ deps ];
              RVLIBC = riscvBuildPackages.glibc;
            };
        };
    };
}
