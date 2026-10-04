#!/bin/sh
# Installs Ixo from a GitHub release (Linux and macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/Eqixo/Ixo/main/install.sh | sh
#
# Settings (environment variables):
#   IXO_VERSION=v0.5.0     a specific release (default: the latest)
#   IXO_HOME=~/.ixo        where it goes (the binary lands in $IXO_HOME/bin)
#   IXO_ARCHIVE=file.tgz   install from a downloaded archive instead
set -eu

REPO=Eqixo/Ixo
IXO_HOME=${IXO_HOME:-"$HOME/.ixo"}

case "$(uname -s)" in
  Linux) os=linux ;;
  Darwin) os=macos ;;
  *) echo "ixo install: unsupported system $(uname -s) (on Windows use install.ps1)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) arch=x86_64 ;;
  arm64|aarch64) arch=arm64 ;;
  *) echo "ixo install: unsupported processor $(uname -m)" >&2; exit 1 ;;
esac
name="ixo-$os-$arch"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ -n "${IXO_ARCHIVE:-}" ]; then
  cp "$IXO_ARCHIVE" "$tmp/$name.tar.gz"
else
  if [ -n "${IXO_VERSION:-}" ]; then
    url="https://github.com/$REPO/releases/download/$IXO_VERSION/$name.tar.gz"
  else
    url="https://github.com/$REPO/releases/latest/download/$name.tar.gz"
  fi
  echo "downloading $url"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --progress-bar "$url" -o "$tmp/$name.tar.gz"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress "$url" -O "$tmp/$name.tar.gz"
  else
    echo "ixo install: needs curl or wget" >&2
    exit 1
  fi
fi

tar xzf "$tmp/$name.tar.gz" -C "$tmp"
src="$tmp/$name"
[ -x "$src/ixo" ] || { echo "ixo install: the archive has no $name/ixo" >&2; exit 1; }

mkdir -p "$IXO_HOME/bin"
rm -rf "$IXO_HOME/docs" "$IXO_HOME/examples"
cp "$src/ixo" "$IXO_HOME/bin/ixo"
for extra in docs examples README.md; do
  [ -e "$src/$extra" ] && cp -R "$src/$extra" "$IXO_HOME/"
done
chmod +x "$IXO_HOME/bin/ixo"

echo "installed $("$IXO_HOME/bin/ixo" version) to $IXO_HOME/bin/ixo"
case ":$PATH:" in
  *":$IXO_HOME/bin:"*) ;;
  *)
    shell_rc="your shell's startup file"
    case "${SHELL:-}" in
      */zsh) shell_rc="~/.zshrc" ;;
      */bash) shell_rc="~/.bashrc" ;;
      */fish) shell_rc="~/.config/fish/config.fish" ;;
    esac
    echo
    echo "add it to your PATH (in $shell_rc):"
    echo "  export PATH=\"$IXO_HOME/bin:\$PATH\""
    ;;
esac
