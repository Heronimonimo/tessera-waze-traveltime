# Changelog

## 0.2.0
- New: the travel time in the top bar. Add **Travel time** to a page's top bar (Top bar, Add, From plugins): "23 min",
  or "23 min +8" when it is slower than usual, with the car in green, amber or red.
- Adding or updating the plugin now asks for the **Waze sensor for the top bar** once per screen.
- The top bar's usual time is a setting of the screen: **Waze usual time** in Home Assistant, and on the screen's
  settings page under Plugins.
- The card asks Waze in the tile's own region in every case.
- The Plugins page now says the plugin uses a service outside the home (Waze, through Home Assistant), with a privacy
  note in the README.

## 0.1.0
- First version: a tile of a Waze Travel Time sensor with the travel time, the delay against your usual time, the
  route, when you arrive and a line of the last hours from the sensor's history.
- A card with every route Waze offers right now, fastest first.
