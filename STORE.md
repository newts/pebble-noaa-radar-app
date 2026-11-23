# Pebble App Store Listing

Copy and assets for the NOAA Radar Zoom appstore listing: what CloudPebble's
Publish pane or `pebble publish` asks for, plus the fields the web dashboard
lets you edit afterwards.

## App name

NOAA Radar Zoom

(Must match `pebble.displayName` in `package.json`, which the PBW carries as
`longName`.)

## Tagline

Live NOAA storm radar with zoom, on your wrist

## Short description

Live NOAA base reflectivity radar over a USGS topographic map, centered on your
location. Zoom from 25 to 1000 km with the buttons, switch between translucent
and opaque radar, recenter and refresh. Updates every minute while open.
United States only.

## Full description

Pull up the storm, not just the forecast.

NOAA Radar Zoom fills your Pebble's screen with live weather radar centered on
where you are: the same NOAA base reflectivity imagery you'd check on your
phone, blended over a USGS topographic map and refreshed every minute while the
app is open.

**Controls**

- **Up / Down**: zoom in and out, from 25 km to 1000 km across the screen
- **Select**: switch the radar between translucent (map shows through) and opaque
- **Hold Select**: recenter on a fresh GPS fix
- **Hold Up**: refresh now
- **Hold Down**: hide or show the center marker and status line for a clean map

**Features**

- Full-screen live radar over a topographic basemap, with a marker at your position
- Eight zoom steps: 25, 50, 100, 150, 250, 400, 600 and 1000 km
- A status line with the zoom and how old the radar is
- The backlight stays on for 30 seconds after each button press, long enough to read the map
- Follows your phone's GPS, or pin it to a fixed latitude and longitude in settings
- Remembers your zoom and radar mode for next time
- A Bluetooth badge appears when your phone is out of range, so you know the map has stopped updating
- Light on the watch: the phone blends and shrinks the imagery to 16 colors, and an unchanged refresh isn't re-sent

Radar and basemap come from NOAA and the USGS National Map, both free, with no
API keys and no accounts.

Based on the NOAA US Weather Radar watchface by Leo Herzog (MIT license).

**Before you install**

- Coverage is **United States only**. These are US government services, so
  there is no imagery outside the country.
- Needs the Pebble phone app for location and networking.
- Works on Pebble Time 2 (emery), Pebble Time / Time Steel (basalt),
  Pebble Time Round (chalk) and Pebble Round 2 (gabbro). Tested on Pebble
  Time 2.

## Category

Daily (where weather apps are listed). Check the dashboard's current category
list when submitting.

## Icons

`screenshots/icon/icon_80x80.png` is the Small Icon and
`screenshots/icon/icon_144x144.png` the Large Icon. Watchapps show their icons
on the listing page and in the Pebble phone app, so both should be uploaded.

The icon in the watch's own app list is the build resource
`resources/images/menu_icon.png`.

## Keywords

weather, radar, noaa, storm, rain, map, zoom, precipitation, severe weather,
hurricane, tornado, meteorology

## Screenshots

**To do:** the files in `screenshots/store/` are from the original watchface
(text lines over the map) and must not be uploaded for this app.

Capture new ones from the CloudPebble emulator (or `pebble screenshot`) on each
platform you list, ideally with weather on screen:

1. Default view (250 km, translucent) with the status line
2. Zoomed in (25 or 50 km) on a storm
3. Opaque radar
4. Overlays hidden: just the map
5. Zoomed out (1000 km) showing a large system

When uploading through `pebble publish`, a filename must start with the
platform name and an underscore (`emery_1_default.png`), since the uploader
infers the platform from it.

## Marketing banner

**To do:** 720×320. The banners in `screenshots/banner/` show the watchface and
need replacing.

## Release notes — v2.0.0

:tada: First release as a watchapp, converted from the NOAA US Weather Radar
watchface:

- Button controls for zoom (eight steps, 25-1000 km), radar mode, recenter,
  refresh and overlays
- Refreshes every minute while open
- Status line with zoom and radar age
- Time, date, health and weather text lines removed

## App information

- **Author / company**: newts (`companyName` in the built PBW)
- **Version**: 2.0.0 (`versionLabel`)
- **UUID**: `7d0cc50d-e4a0-4cb4-9ec6-1329216d8ea0`
- **Type**: watchapp
- **Platforms**: emery, basalt, gabbro, chalk
- **License**: MIT
- **Source URL**: `https://github.com/newts/pebble-noaa-radar-app`
- **Support email**: to be chosen
