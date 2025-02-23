{
  stdenv,
}:
stdenv.mkDerivation {
  pname = "playground";
  version = "0.1.0.0";

  src = ./../..;
}
