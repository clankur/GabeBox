# ANGLE graphics control on Xbox

The Stardew Valley diagnostic reached `MonoGame.OpenGL.GL.LoadExtensions` without a current OpenGL context. Loading system `opengl32.dll` returned `0x7E`. A Direct3D 11 hardware feature-level query returned `11_0`. This control uses the pinned ANGLE UWP build to compile OpenGL ES shaders and draw into an offscreen EGL pbuffer over that Direct3D backend. It runs without Steam or game files. [Stardew graphics evidence](https://github.com/clankur/steam-xbox-devmode/issues/32) · [ANGLE platform setup](https://github.com/google/angle/blob/main/doc/DevSetup.md)

Download `xbox-angle-uwp-x64` and `xbox-steam-core-template` from the two workflow runs for the same branch revision. Verify their checksum manifests. Prepare and sign a package with a version above the currently installed Steam Core probe.

```sh
python3 probe/prepare-steam-core.py "$template" "$trial_dir/layout" \
  --version "$version" --angle-control "$angle_artifact" --vclibs-framework "$uwp_framework"
bash probe/package.sh --layout "$trial_dir/layout" "$trial_dir/package"
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/package/PackagedProcessProbe-x64.appx" --dependency "$uwp_framework" --test-probe --timeout 45
```

Success requires `angle_stage=11`, `angle_control=green-pixel`, and `angle_rgba=0,255,0,255`. Stages 1–3 cover loading the platform entry point, choosing hardware D3D11, and initializing EGL. Stages 4–7 cover config, pbuffer, context and bind. Stages 8–10 cover compiling shaders, linking and drawing. A successful pixel establishes offscreen OpenGL ES rendering. Desktop OpenGL entry points, window presentation and individual game integration have separate acceptance tests.

## Desktop OpenGL trial

The pinned Mesa UWP release includes the desktop `opengl32.dll` exports used by Steam builds. Stage it in a fresh signed diagnostic layout with `stage-mesa-uwp.py` before packaging. Use the original `alpha-2-resfix.zip`, the license from the pinned source revision, and the official `Microsoft.VCLibs.140.00` x64 framework. Supply that framework as a dependency alongside any existing desktop Visual C++ framework. The script verifies the source archive and license digests, copies the named runtime libraries, and limits game trials to the configured titles. This stages a local app dependency for an explicit test. [Mesa UWP release](https://github.com/aerisarn/mesa-uwp/releases/tag/alpha-2-resfix) · [Pinned source and license](https://github.com/aerisarn/mesa-uwp/tree/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b)
