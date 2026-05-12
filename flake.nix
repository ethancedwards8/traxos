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

              (stdenv.mkDerivation {
                  pname = "tnt";
                  version = "0.1.0";

                  src = fetchFromGitHub {
                      owner = "sujanan";
                      repo = "tnt";
                      rev = "a890c7523355b7e1210e810e6a108655ab5b11d8";
                      hash = "sha256-DqLdg1oqil6bTghNTWjqayXk2QkQDfZfWZ9kFvM/+/A=";
                  };

                  makeFlags = [ "CC=cc" ];

                  nativeBuildInputs = [ installShellFiles ];

                  installPhase = ''
                    runHook preInstall

                    installBin tnt

                    runHook postInstall
                  '';
              })
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
