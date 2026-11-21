# Steam Big Picture on Xbox

Valve ships Big Picture as a mode of the desktop Steam client. The Xbox application authenticates with Steam and drives its library through a packaged Steam Core subset. The app provides its own sign-in and library screens. [Valve’s Big Picture support page](https://help.steampowered.com/en/faqs/view/3725-76D3-3F31-FB63) · [Steam Core probe](../probe/STEAM-CORE.md)

## Release evidence

Valve’s Windows x64 client manifest `1794536281` lists the `steamui_websrc_all` archive at 35,981,544 bytes with SHA-256 `c4ef0ef15369de9076657e67af5b218d95d71402ed3f92a1b3d096fb06a7b69b`. The downloaded archive matched. Its `steamui/index.html` loads the desktop library bundle. Served unchanged over loopback to a clean Chromium profile, that entry stopped in `library.js` with `ReferenceError: SteamClient is not defined`. The `steamui/sp.js` bundle contains the gamepad UI routes and calls into Steam’s native `SteamClient` bridge, including the UI mode, account, controller input, applications and downloads namespaces. A static scan of the entire shared desktop and gamepad bundle found 709 distinct two-part native bindings. This count describes the bundled API surface. The startup path for the gamepad window would need its own trace. [Valve client manifest](https://client-update.akamai.steamstatic.com/steam_client_win64)

The desktop boot path also uses `steam.exe`, `SteamUI.dll`, and `steamwebhelper.exe`. The checked client binaries import desktop window APIs and Chromium Embedded Framework entry points. The packaged Xbox host currently stages `steamclient64.dll`, `tier0_s64.dll` and `vstdlib_s64.dll`. Even with a Chromium-compatible renderer, Valve’s actual UI would require its native window and client-service bridge. [Measured imports](client-imports.json) · [Packaged core builder](../probe/prepare-steam-core.py)

## Post-sign-in Xbox library

The Xbox library fills the display after either a restored session or a new sign-in. Its controller-friendly artwork grid reuses the existing Steam library data, artwork cache, and Play and Download actions. D-pad navigation follows the visible rows and columns. Select a tile to focus its action. Back returns focus from the action to the selected title. The presentation uses the app’s own layout and includes no Valve client UI assets.

A local Chromium test with a fixture session verified post-sign-in rendering, movement across rows, action focus, the selected AppID reaching Play, and search showing the Download action for an uninstalled title. On Xbox Series X build `1.0.220.0`, the cached Steam session restored and the Device Portal screenshot showed the fullscreen Xbox library grid with artwork and its selected title's Download action.
