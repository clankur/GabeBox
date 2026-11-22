"""Stage the production XAML host and WebView2 renderer in an account-free test package."""
import argparse
import hashlib
from pathlib import Path
import runpy
import shutil


def prepare(layout, primary, primary_pin, renderer, renderer_pin, version):
    source = Path(__file__).resolve().parent
    if hashlib.sha256((renderer / "SHA256SUMS").read_bytes()).hexdigest() != renderer_pin:
        raise ValueError("Unexpected WebView2 renderer checksum manifest")
    runpy.run_path(str(source.parent / "big-picture-primary/prepare-smoke.py"))["prepare"](
        layout, primary, primary_pin, version)
    manifest = layout / "AppxManifest.xml"
    text = manifest.read_bytes()
    previous = b'Name="XboxPortabilityProbe.SteamPrimaryControl"'
    if text.count(previous) != 1:
        raise ValueError("Unexpected control package identity")
    manifest.write_bytes(text.replace(previous, b'Name="XboxPortabilityProbe.PrimaryRendererControl"')
                         .replace(b"Steam Primary Activation Control", b"Primary Renderer Control"))
    runpy.run_path(str(source.parent / "big-picture-webview/stage.py"))["add_webview_host"](layout, renderer)
    shutil.copyfile(source / "account.js", layout / "activation-check.js")
    content = layout / "ValveClient/steamui"
    content.mkdir(parents=True)
    shutil.copyfile(source / "browser.html", content / "index.html")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("layout", type=Path)
    parser.add_argument("primary", type=Path)
    parser.add_argument("primary_pin")
    parser.add_argument("renderer", type=Path)
    parser.add_argument("renderer_pin")
    parser.add_argument("--version", default="1.0.0.1")
    args = parser.parse_args()
    prepare(args.layout, args.primary, args.primary_pin, args.renderer, args.renderer_pin, args.version)
