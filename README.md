# NOAA US Weather Radar (watchapp) ⛈️

A Pebble **watchapp** that fills the screen with a live NOAA weather radar map centered on your location, with button controls for zoom, radar mode, recentering and refresh. Forked from Leo Herzog's [NOAA US Weather Radar watchface](https://github.com/leoherzog/pebble-noaa-radar-watchface) and turned from a watchface into an app.

Your phone does the heavy lifting: it finds your location, fetches the NOAA base reflectivity layer and a USGS topographic basemap, blends them, squeezes the result down to a 16-color image, and streams it over Bluetooth. The watch just draws it.

## Controls

| Button | Action |
|--------|--------|
| Up | Zoom in |
| Down | Zoom out |
| Select | Switch radar between Translucent and Opaque |
| Hold Select | Recenter (takes a fresh GPS fix) |
| Hold Up | Refresh now |
| Hold Down | Hide or show the overlays (center marker and status line) |
| Back | Exit |

Zoom steps across the screen width: 25, 50, 100, 150, 250 (default), 400, 600 and 1000 km. Zoom and radar mode are remembered between launches.

The status line at the bottom shows the zoom and how old the radar is (`250 km | 3 min`), or `Loading...` while a change is being fetched. A short message pops up in the middle when the zoom or mode changes. A crossed-out Bluetooth badge appears when the phone is out of reach.

## How it updates

- The map refreshes once a minute while the app is open. If the radar hasn't changed, nothing is re-sent to the watch; the age just resets.
- A zoom or radar-mode change fetches a new frame right away. Zoom changes throw away the cached basemap.
- Nothing is saved for startup: the app opens grey and fills in when the first frame arrives (usually a few seconds).
- The watch holds one image at a time.

Radar and basemap data come from [NOAA](https://mapservices.weather.noaa.gov/) and the [USGS National Map](https://basemap.nationalmap.gov/), free and with no API keys. Both are US-only.

## Building

```bash
npm install
pebble build
pebble install --phone <phone-ip>      # or: pebble install --emulator emery
```

## Requirements

- A color Pebble: Pebble Time 2 (`emery`, the primary target), Pebble Time / Time Steel (`basalt`), Pebble Time Round (`chalk`) or Pebble Round 2 (`gabbro`)
- A location in the United States
- The Pebble phone app, for location and networking

## Settings

Open the app's settings in the Pebble phone app to choose between following the phone's GPS or a fixed latitude/longitude. Everything else is on the watch buttons.

## License

The MIT License (MIT)

Copyright © 2026 Leo Herzog

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## About Me

<a href="https://herzog.tech/" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/link-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/link.svg.png">
    <img src="https://herzog.tech/signature/link.svg.png" width="32px">
  </picture>
</a>
<a href="https://mastodon.social/@herzog" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/mastodon-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/mastodon.svg.png">
    <img src="https://herzog.tech/signature/mastodon.svg.png" width="32px">
  </picture>
</a>
<a href="https://github.com/leoherzog" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/github-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/github.svg.png">
    <img src="https://herzog.tech/signature/github.svg.png" width="32px">
  </picture>
</a>
<a href="https://keybase.io/leoherzog" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/keybase-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/keybase.svg.png">
    <img src="https://herzog.tech/signature/keybase.svg.png" width="32px">
  </picture>
</a>
<a href="https://www.linkedin.com/in/leoherzog" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/linkedin-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/linkedin.svg.png">
    <img src="https://herzog.tech/signature/linkedin.svg.png" width="32px">
  </picture>
</a>
<a href="https://hope.edu/directory/people/herzog-leo/" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/anchor-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/anchor.svg.png">
    <img src="https://herzog.tech/signature/anchor.svg.png" width="32px">
  </picture>
</a>
<br />
<a href="https://herzog.tech/$" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://herzog.tech/signature/mug-tea-saucer-solid-light.svg.png">
    <source media="(prefers-color-scheme: light)" srcset="https://herzog.tech/signature/mug-tea-saucer-solid.svg.png">
    <img src="https://herzog.tech/signature/mug-tea-saucer-solid.svg.png" alt="Buy Me A Tea" width="32px">
  </picture>
  Found this helpful? Buy me a tea!
</a>
