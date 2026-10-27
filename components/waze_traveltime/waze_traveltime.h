#pragma once
#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::waze_traveltime {

// A part that waits for an answer of the app to tessera::send(): a tile's history, the card's routes.
class Asker {
 public:
  virtual void answer(JsonObjectConst message) = 0;
};

// The trip of a Waze sensor, for the card opened for it: the region of the tile that shows it, and Waze's two ends as
// the sensor has them ("52.09,5.12") once they came. Fixed fields, so it needs no memory of its own.
struct Trip {
  char entity[64] = "";
  char origin[64] = "";
  char destination[64] = "";
  char region[4] = "eu";
};

// The plugin: the tile "travel" of a Waze Travel Time sensor, the card "routes", the top bar's item "travel" and a row
// for the usual time on the screen's settings page.
class WazeTraveltime : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  bool settings(tessera::SettingsPage &page) override;
  void on_message(JsonObjectConst message) override;

  void set_minutes(sensor::Sensor *s) { minutes_ = s; }
  void set_usual(number::Number *n) { usual_ = n; }

  // Asks the app, and hands the answer to `asker` when it comes; 0 when it could not be sent.
  uint32_t ask(Asker *asker, JsonObjectConst request);
  // An asker that goes away: its answer, when it still comes, goes nowhere.
  void forget(Asker *asker);

  // The tiles tell the plugin each sensor's region when they are made and its two ends as they come, so a card opened
  // for that sensor knows where to ask for. A few sensors at once; the oldest makes room. `origin` and `destination`
  // may be null: the region alone.
  void remember(const char *entity, const char *origin, const char *destination, const char *region);
  const Trip *trip(const char *entity) const;

 protected:
  tessera::BarItem bar() const;

  sensor::Sensor *minutes_{};
  number::Number *usual_{};
  Trip trips_[4];
  int next_trip_ = 0;
  struct Waiting {
    uint32_t re = 0;
    Asker *asker = nullptr;
  };
  Waiting waiting_[8];
};

}  // namespace esphome::waze_traveltime
