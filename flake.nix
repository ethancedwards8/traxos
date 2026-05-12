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
              transmission_4

              (buildGoModule {
                  pname = "torrent-client";
                  version = "0.1.0";

                  src = fetchFromGitHub {
                      owner = "veggiedefender";
                      repo = "torrent-client";
                      rev = "e0f58e0b16e4a9d1bd889f8cb61d835b8ec13385";
                      hash = "sha256-bNgpu3cAuYhTTwIb9eaw92TijIIVSDH6qwsl4eBb7lM=";
                  };

                  vendorHash = "sha256-gsCUXahsTf2OUT8o/sxr3pLv3tosSGS3FvM10CRFJoQ=";

                  doCheck = false;
              })

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

              latexrun
              texliveFull
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
