<p align="center">
  <img src="src/pluma.png" width="96" alt="Pluma">
</p>

<h1 align="center">Pluma</h1>

<p align="center"><b>A web browser for Windows that holds almost nothing in memory.</b><br>
Edge's engine (WebView2) behind a native interface of its own.</p>

<p align="center">Made by <b>Samuel</b> (<a href="https://github.com/samuelbbp">@samuelbbp</a>).</p>

---

- **Measured**: one ordinary tab, 117-140 MB; seven tabs, ~125 MB (Firefox with the same seven: 3,100 MB);
  YouTube playing at 1080p, ~110-150 MB in Pluma's player (385 MB in YouTube's own page, the default).
- **How**: tabs out of sight are frozen after 45 s, and come back at once with everything they had (unloading them
  after a while is an option in the settings; only a PC short of memory lets the oldest go); processes with nothing
  on screen give their unused memory back to Windows.
- **YouTube in its own player** (optional, off by default): videos play in Pluma's player (libmpv) in their place in
  the page, with YouTube's controls, keys and qualities up to 4K, while the heavy page sleeps behind it. A click or the
  wheel on the page wakes it at the same second.
- **Reader view**: the book in the address bar (or F9) on an article shows just its text and pictures, light or dark,
  with the font size you choose; a site can open its articles in reader view always. Uses Mozilla's
  [Readability](https://github.com/mozilla/readability) (Apache-2.0, `src/readability`).
- **Translation**: a page in another language lights the translate icon in the address bar and offers to translate it,
  as Edge does; text that appears later is translated too, and "Show original" puts the page back. Selected text:
  right click > Translate, in a small bubble. Google Translate, nothing sent until you ask.
- **Built in**: incognito tabs, uBlock Origin Lite, Chrome extensions from the Web Store, user scripts (Violentmonkey style),
  saved passwords, downloads, find, zoom, print.
- **Native**: C++17, Win32, Direct2D and DirectWrite, one ~1 MB executable. Light, dark or as Windows (the default);
  the bar's font to choose (Windows's own by default); the new tab shows the pages you add with the address's star
  (Ctrl+D).
- **Languages**: Spanish, English, Portuguese, French, German and Italian; Windows's language by default.
- **Updates**: automatic from this repo's releases (checked against their SHA-256, installed when the window closes), or just a notice, or off.

## Install

Download `pluma-vX.Y.Z.zip` from the latest release, unzip it and run `instalar.ps1` (`-ArrancarConWindows` to have it open instantly from sign-in). It adds
shortcuts, registers Pluma as a browser (choose it as the default in Windows' settings) and fetches the video engine.
No administrator rights. It needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11.

## Build

`src\compilar.cmd` with the Visual Studio Build Tools (C++); the WebView2 SDK downloads itself the first time.
Every push to `main` builds and publishes a release (`[skip release]` in the commit skips it); pull requests are built
by the CI.

---

<p align="center"><sub>© 2026 Samuel (<a href="https://github.com/samuelbbp">@samuelbbp</a>). Part of
<a href="https://github.com/Windows-Lite">Windows Lite</a>.</sub></p>
