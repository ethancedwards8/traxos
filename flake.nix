{
    description = "student's cs2620 flake";

    inputs = {
        nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    };

    outputs = inputs@{ self, nixpkgs, ... }:
    let
        inherit (self) outputs;
        forAllSystems = nixpkgs.lib.genAttrs nixpkgs.lib.systems.flakeExposed;
    in
    {
      devShell = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        with pkgs;
        mkShell {
          name = "dev shell";
          nativeBuildInputs = [
              cmake
              llvmPackages.clang
              mktorrent
          ];

          buildInputs = [
              grpc
              openssl
              protobuf
              xxhash
          ];
        }
      );
    };
}
