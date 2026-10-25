<p align="center">
  <img src="src/pluma.png" width="96" alt="Pluma">
</p>

<h1 align="center">Pluma</h1>

<p align="center"><b>A web browser for Windows that holds almost nothing in memory.</b><br>
Edge's engine (WebView2) behind a native interface of its own.</p>

<p align="center">Made by <b>Samuel</b> (<a href="https://github.com/samuelbbp">@samuelbbp</a>).</p>

---

- **Measured**: one ordinary tab, 117-140 MB; seven tabs, ~125 MB (Firefox with the same seven: 3,100 MB);
  YouTube playing at 1080p, ~110-150 MB (385 MB in YouTube's own page).
- **How**: tabs out of sight are frozen after 45 s and let go after a minute (the last ones looked at stay frozen
  for 20 minutes and come back at once, where they were); processes with nothing on screen give their unused memory
  back to Windows.
- **YouTube in its own player**: videos play in Pluma's player (libmpv) in their place in the page, with YouTube's
  controls, keys and qualities up to 4K, while the heavy page sleeps behind it. A click or the wheel on the page wakes
  it at the same second.
- **Built in**: incognito tabs, uBlock Origin Lite, Chrome extensions from the Web Store, user scripts (Violentmonkey style),
  saved passwords, downloads, find, zoom, print.
- **Native**: C++17, Win32, Direct2D and DirectWrite, one ~1 MB executable. Dark by default; the new tab shows the
  pages you add with the address's star (Ctrl+D).

## Install

Clone the repository and run `instalar.ps1` (`-ArrancarConWindows` to have it open instantly from sign-in). It adds
shortcuts, registers Pluma as a browser (choose it as the default in Windows' settings) and fetches the video engine.
No administrator rights. It needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11.

## Build

`src\compilar.cmd` with the Visual Studio Build Tools (C++); the WebView2 SDK downloads itself the first time.

---

<p align="center"><sub>© 2026 Samuel (<a href="https://github.com/samuelbbp">@samuelbbp</a>). Part of
<a href="https://github.com/Windows-Lite">Windows Lite</a>.</sub></p>
