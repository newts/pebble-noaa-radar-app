// The Clay settings page. Zoom and radar mode are set from the watch buttons,
// so the page holds only the location source. UseGps and ManualLoc are
// phone-side, not package.json messageKeys: webviewclosed folds them into the
// localStorage key 'ManualLoc' ('' = GPS).

// Section headings default to h4, so the page title is bumped one level up to
// keep the section headings reading as subordinate to it.
module.exports = [
  {
    "type": "heading",
    "size": 3,
    "defaultValue": "NOAA US Weather Radar"
  },
  {
    "type": "text",
    "defaultValue":
      "On the watch: <b>Up</b>/<b>Down</b> zoom, <b>Select</b> switches the radar " +
      "between Translucent and Opaque. Hold <b>Select</b> to recenter, hold <b>Up</b> " +
      "to refresh, hold <b>Down</b> to hide or show the overlays."
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Location"
      },
      // custom-clay.js hides ManualLoc while UseGps is on, shows ManualLocError
      // and disables Save until the text parses as a coordinate pair.
      {
        "type": "toggle",
        "messageKey": "UseGps",
        "label": "Use GPS",
        "defaultValue": true,
        "description": "Center the map on the phone's location."
      },
      {
        "type": "input",
        "messageKey": "ManualLoc",
        "label": "Latitude, Longitude",
        "defaultValue": "",
        "attributes": { "placeholder": "e.g. 40.69, -74.04" },
        "description": "Decimal degrees, as copied from a long-press in most map apps."
      },
      {
        "type": "text",
        "id": "ManualLocError",
        "defaultValue": "&#9888; Enter as latitude, longitude in decimal degrees (latitude −90 to 90, longitude −180 to 180)."
      }
    ]
  },
  {
    // Attribution. The NSSL and USGS links block or time out for non-browser
    // clients, so a link checker reports them dead; they resolve in a browser.
    "type": "text",
    "defaultValue":
      "Radar data by <a href='https://www.nssl.noaa.gov/projects/mrms/' target='_blank'>NOAA</a> " +
      "and basemaps by the <a href='https://www.usgs.gov/programs/national-geospatial-program/national-map' target='_blank'>USGS</a>. " +
      "Based on the NOAA US Weather Radar watchface by Leo Herzog."
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
