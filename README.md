# Waze travel time

Your travel time from Waze on a Tessera screen: how long the trip takes now, how far that is off your usual time, the
route Waze picks and when you arrive, with a line of how the travel time went over the last hours. A tap opens a card
with the routes Waze offers right now (up to six), fastest first.

It reads a sensor of Home Assistant's own [Waze Travel Time](https://www.home-assistant.io/integrations/waze_travel_time)
integration. No key, no account and no web service of its own: Home Assistant asks Waze, the plugin asks Home Assistant.

## Set up

1. In Home Assistant, add the **Waze Travel Time** integration (Settings, Devices and services, Add integration) with
   your two places, for example `zone.home` and your work address. It makes a sensor such as
   `sensor.home_to_work`. Keep the **Recorder** on for it (it is by default): the line on the tile comes from its
   history.
2. In Tessera, open **Plugins** and choose **Waze travel time**, or choose **Add with a link** and paste
   `https://github.com/Heronimonimo/tessera-waze-traveltime`. Tick your screens and choose the **Waze sensor for the
   top bar** (asked once per screen, also when you don't use the top bar); each screen builds once with the plugin.
3. Put the tile **Travel time** on a page, at any size from 1x1 to 4x4, and choose the Waze sensor in its inspector.
4. In the inspector, set:
   - **Usual travel time**: your time without traffic, in minutes. The tile then shows the delay ("+8") in green,
     amber or red, and the line gets a dashed line at that time. Leave it at 0 for no delay.
   - **Hours on the line**: how far back the line goes, 1 to 8 hours (0 for no line).
   - **Waze region**: the same region as your Waze Travel Time integration, for the routes on the card.
5. Optional: add **Travel time** to a page's top bar (Top bar, Add, From plugins). It shows the sensor you chose in
   step 2. For a coloured car, set the usual time: hold the top bar, then **Settings**, **Plugins**, **Waze travel
   time**, **Usual travel time**, or the entity **Waze usual time** of the screen in Home Assistant.

## On the screen

- **1x1**: the minutes, the delay top right, and when you arrive.
- **2x1**: the same, with the route ("via A12").
- **2 rows or more**: the line of the last hours under it, in green, amber or red by the delay now (the accent colour
  without a usual time), and the usual time dashed.
- **Tap**: the card with the routes Waze offers now, as many as fit (up to six), each with its time, a bar, its distance and how much slower it is
  than the fastest.
- **Top bar**: "23 min" beside the page's name, or "23 min +8" when it is slower than the usual time, with the car in
  green, amber or red by the delay (grey without a usual time). Hidden while the sensor has no travel time. A page can
  show its icon alone.

## Good to know

- The line comes from the sensor's history in Home Assistant (`history/history_during_period`), asked once when the
  tile is made; every new travel time after that is added on the screen. The sensor updates every 5 minutes by
  default, so the line moves in steps of about 5 minutes.
- The card asks Waze through Home Assistant's `waze_travel_time.get_travel_times` action each time it opens, with live
  traffic. It uses the sensor's own start and end, but not the sensor's other options (avoid tolls, filters). Distances
  on the card are in km.
- Waze Travel Time is an unofficial client of Waze's live map: when Waze changes something, the integration (and so
  this plugin) may stop until Home Assistant fixes it.
- Works on every board, the CYD included: it shows no pictures.
- Tried on Tessera's preview bench (the CYD, the Guition 4-inch, the Waveshare 4.3-inch standing up and the Guition
  10.1-inch, light and dark), not yet on a real screen. Found something wrong? Open an issue.

## Privacy

The plugin itself reaches nothing outside your home: the screen talks to Home Assistant only. Home Assistant's Waze
Travel Time integration asks Waze's live map for the travel time, every 5 minutes, and once more each time the card
opens. Waze then sees the two ends of your trip (often your home and your work, as coordinates) and the address of your
Home Assistant. No account, key or other personal data is sent. What Waze does with it is in
[Waze's privacy policy](https://www.waze.com/legal/privacy).

## How it works

- `tessera-plugin.yaml`: a tile of a sensor with the attributes `route`, `distance`, `origin` and `destination`, a card,
  a top bar item, the input that chooses the top bar's sensor, the setting of its usual time, and the two Home Assistant
  commands it may ask, with the history mapped to two lists of numbers.
- `plugin.yaml`: a `homeassistant` sensor that reads the chosen sensor for the top bar, and the usual time as a
  template number.
- `components/waze_traveltime/`: the tile (the line is drawn on the screen from the samples), the card, the top bar
  item, the settings row, and the plugin that hands each answer to the tile or card that asked.
