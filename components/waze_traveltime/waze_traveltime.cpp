#include "waze_traveltime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace esphome::waze_traveltime {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

static constexpr int SAMPLES = 96;       // the states the line keeps: 8 hours of a sensor that changes every 5 minutes
static constexpr uint32_t RETRY = 300;   // seconds before a history that failed is asked again

// ---- Words and numbers ----

// Minutes as the tile writes them: whole.
static std::string whole(float minutes) {
  char b[12];
  snprintf(b, sizeof b, "%ld", lroundf(minutes));
  return b;
}

// A distance as "18.4 km".
static std::string km(const tessera::Plugin *plugin, float distance) {
  char b[16];
  snprintf(b, sizeof b, "%.1f", distance);
  return tessera::fill(plugin->text("km"), "n", b);
}

// How bad a delay against the usual time is: within two minutes fine, within ten busy, worse an alert.
static Tone tone_of(float delay) { return delay <= 2 ? Tone::GOOD : delay <= 10 ? Tone::BUSY : Tone::ALERT; }

// A moment in UTC as Home Assistant's history takes it: "2026-10-19T06:12:00+00:00".
static std::string iso(uint32_t epoch) {
  const time_t t = epoch;
  struct tm u;
  gmtime_r(&t, &u);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S+00:00", &u);
  return b;
}

// A sensor's state as minutes; NaN for "unavailable", "unknown" or nothing.
static float minutes_of(const char *state) {
  if (!state || !*state) return NAN;
  char *end = nullptr;
  const float v = strtof(state, &end);
  return end != state && std::isfinite(v) && v >= 0 ? v : NAN;
}

// ---- The tile: the travel time now, the delay, the route and the line of the last hours ----
class TravelTile : public tessera::Tile, public Asker {
 public:
  explicit TravelTile(WazeTraveltime *plugin) : plugin_(plugin) {}
  ~TravelTile() override { plugin_->forget(this); }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    wide_ = c.columns >= 2;
    entity_ = c.entity ? c.entity : "";
    title_ = c.name ? c.name : "";
    usual_ = c.options["usual"] | 0.0f;
    hours_ = std::clamp(static_cast<int>(lroundf(c.options["hours"] | 3.0f)), 0, 8);  // 3 or 3.0, as the app kept it
    const char *region = c.options["region"] | "eu";
    snprintf(region_, sizeof region_, "%s", region);
    // The sensor's region for a card opened for it, also before its two ends came.
    if (!entity_.empty()) plugin_->remember(entity_.c_str(), nullptr, nullptr, region_);

    const int body = ui::line_height(Font::BODY);
    // The line takes the lower part of a tile of two rows or more, its times under it.
    graph_h_ = hours_ > 0 && c.rows >= 2 ? height_ * 2 / 5 : 0;
    // The largest face for the number that leaves room for the name above and the route under it.
    const int room = height_ - 2 * body - (graph_h_ ? graph_h_ + ui::px(4) : 0);
    face_ = Font::BODY_LARGE;
    for (Font f : {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE})
      if (ui::line_height(f) <= room) { face_ = f; break; }
    const int big = ui::line_height(face_);

    name_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_pos(name_, 0, 0);
    delta_ = ui::label(c.parent, Font::BODY, theme::INK);
    lv_obj_set_pos(delta_, width_, 0);
    // The delay's colour as a dot before it: coloured words would be too faint on the light look.
    dot_ = ui::block(c.parent, theme::MUTED);
    const int dot = ui::px(8);
    lv_obj_set_size(dot_, dot, dot);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(dot_, width_ - dot, (body - dot) / 2);
    lv_obj_set_style_opa(dot_, LV_OPA_TRANSP, 0);
    number_ = ui::label(c.parent, face_, theme::INK);
    lv_obj_set_pos(number_, 0, body);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    info_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(info_, width_);
    // Under the number, or under the name when there is no room left (the smallest tile of a compact screen).
    info_y_ = std::min(body + big, height_ - body - graph_h_);
    lv_obj_set_pos(info_, 0, info_y_);
    number_y_ = body;

    if (graph_h_) {
      const int line_h = graph_h_ - body;
      graph_ = lv_obj_create(c.parent);
      lv_obj_remove_style_all(graph_);
      lv_obj_remove_flag(graph_, LV_OBJ_FLAG_CLICKABLE);  // a tap on the line opens the card, as on the rest
      lv_obj_remove_flag(graph_, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_size(graph_, width_, line_h);
      lv_obj_set_pos(graph_, 0, height_ - graph_h_);
      lv_obj_add_event_cb(graph_, draw, LV_EVENT_DRAW_MAIN, this);
      from_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_width(from_, width_ / 2);
      lv_obj_set_pos(from_, 0, height_ - body);
      now_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_width(now_, width_ / 2);
      lv_obj_set_style_text_align(now_, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(now_, width_ - width_ / 2, height_ - body);
    }
  }

  void on_state(JsonObjectConst data) override {
    chosen_ = !data["state"].isNull();
    minutes_ = minutes_of(data["state"] | "");
    name_text_ = data["name"] | "";
    JsonObjectConst a = data["attributes"];
    route_ = a["route"] | "";
    const char *origin = a["origin"] | "", *destination = a["destination"] | "";
    if (*origin && *destination && !entity_.empty()) plugin_->remember(entity_.c_str(), origin, destination, region_);
    // Every new travel time goes on the line too, so it moves on between two asks of the history.
    const uint32_t now = tessera::epoch();
    if (now && std::isfinite(minutes_) && (count_ == 0 || samples_[count_ - 1].minutes != minutes_)) {
      add(now, minutes_);
      if (graph_) lv_obj_invalidate(graph_);
    }
  }

  void on_tick(uint32_t epoch) override {
    // The history of the last hours, once the sensor and the clock are there; again later when it failed.
    if (graph_ && !asked_ && chosen_ && epoch && epoch >= retry_at_ && !entity_.empty()) ask_history(epoch);

    ui::set_text(name_, roomy::plain(title_.empty() ? name_text_ : title_));
    if (!chosen_ || !std::isfinite(minutes_)) {
      ui::set_text(number_, "");
      ui::set_text(unit_, "");
      ui::set_text(delta_, "");
      lv_obj_set_style_opa(dot_, LV_OPA_TRANSP, 0);
      ui::set_text(info_, plugin_->text(chosen_ ? "no_time" : "choose"));
      lv_obj_set_pos(info_, 0, number_y_);
      paint_times(epoch);
      return;
    }
    lv_obj_set_pos(info_, 0, info_y_);

    // The number in the largest face that fits beside its unit.
    const std::string number = whole(minutes_), unit = plugin_->text("min");
    const int gap = ui::px(4), unit_w = ui::text_width(unit, Font::BODY);
    Font face = face_;
    for (Font f : {face_, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      if (ui::line_height(f) > ui::line_height(face_)) continue;
      face = f;
      if ((f != Font::DIGITS || ui::covers(number, f)) && ui::text_width(number, f) + gap + unit_w <= width_) break;
    }
    ui::set_font(number_, face);
    ui::set_text(number_, number);
    const int number_w = ui::text_width(number, face) + ui::px(2), big = ui::line_height(face);
    lv_obj_set_width(number_, number_w);
    lv_obj_set_pos(number_, 0, number_y_ + (ui::line_height(face_) - big));
    ui::set_text(unit_, unit);
    lv_obj_set_width(unit_, unit_w + ui::px(2));
    // The unit on the number's baseline, near enough: at the bottom of its line.
    lv_obj_set_pos(unit_, number_w + gap, number_y_ + ui::line_height(face_) - ui::line_height(Font::BODY) - big / 10);

    // The delay against the usual time, top right, after a dot in the colour of how bad it is.
    tone_ = Tone::ACCENT;
    std::string delta;
    if (usual_ > 0) {
      const float delay = minutes_ - usual_;
      tone_ = tone_of(delay);
      const long n = lroundf(std::fabs(delay));
      delta = n == 0 ? std::string(plugin_->text("usual"))
                     : tessera::fill(plugin_->text(delay > 0 ? "late" : "early"), "n", std::to_string(n));
    }
    ui::set_text(delta_, delta);
    const int dot = ui::px(8);
    const int text_w = delta.empty() ? 0 : ui::text_width(delta, Font::BODY) + ui::px(2);
    const int delta_w = delta.empty() ? 0 : text_w + gap + dot;
    lv_obj_set_width(delta_, std::max(1, text_w));
    lv_obj_set_pos(delta_, width_ - text_w, 0);
    ui::set_fill(dot_, tone_, theme::MUTED);
    lv_obj_set_style_opa(dot_, delta.empty() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_x(dot_, width_ - delta_w);
    lv_obj_set_width(name_, std::max(1, width_ - delta_w - (delta_w ? gap : 0)));

    // The route and when you are there: both on a wide tile, the arrival alone on a narrow one.
    std::string info;
    const std::string arrive = epoch ? tessera::fill(plugin_->text("arrive"), "time",
                                                     tessera::clock_text(epoch + static_cast<uint32_t>(minutes_ * 60)))
                                     : "";
    if (wide_ && !route_.empty()) {
      info = tessera::fill(plugin_->text("via"), "route", roomy::plain(route_));
      if (!arrive.empty()) info += " · " + arrive;
    } else {
      info = arrive;
    }
    ui::set_text(info_, info);
    paint_times(epoch);
  }

  void on_theme() override {
    ui::set_color(name_, theme::MUTED);
    ui::set_color(number_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    ui::set_color(info_, theme::MUTED);
    ui::set_color(delta_, theme::INK);
    ui::set_fill(dot_, tone_, theme::MUTED);
    if (from_) ui::set_color(from_, theme::SUBTLE);
    if (now_) ui::set_color(now_, theme::SUBTLE);
    if (graph_) lv_obj_invalidate(graph_);
  }

  // The routes of the sensor; nothing while no sensor is chosen, as there is nothing to ask for.
  void on_tap() override {
    if (!entity_.empty() && chosen_) tessera::open_card(plugin_->plugin_id(), "routes", roomy::plain(entity_), tile_);
  }

  // The answer of history/history_during_period, mapped by the manifest's `answers`: {"minutes": [...], "at": [...]}.
  void answer(JsonObjectConst message) override {
    asked_ = false;
    if (!(message["ok"] | false)) {
      retry_at_ = tessera::epoch() + RETRY;
      return;
    }
    JsonArrayConst minutes = message["result"]["minutes"], at = message["result"]["at"];
    const int n = std::min<int>(minutes.size(), at.size());
    // What the tile heard since it asked stays: it is newer than the history.
    Sample heard[SAMPLES];
    int kept = 0;
    for (int i = 0; i < count_; ++i) heard[kept++] = samples_[i];
    count_ = 0;
    for (int i = std::max(0, n - SAMPLES); i < n; ++i) {
      if (minutes[i].isNull() || at[i].isNull()) continue;  // a moment the sensor had no value: a gap in the line
      add(static_cast<uint32_t>(at[i].as<double>()), minutes[i].as<float>());
    }
    for (int i = 0; i < kept; ++i) add(heard[i].at, heard[i].minutes);
    if (graph_) lv_obj_invalidate(graph_);
  }

 private:
  struct Sample {
    uint32_t at;
    float minutes;
  };

  void ask_history(uint32_t epoch) {
    JsonDocument request;
    request["ask"] = "history/history_during_period";
    request["data"]["start_time"] = iso(epoch - static_cast<uint32_t>(hours_) * 3600u);
    request["data"]["entity_ids"].add(roomy::plain(entity_));
    request["data"]["minimal_response"] = true;
    request["data"]["no_attributes"] = true;
    request["data"]["significant_changes_only"] = false;
    asked_ = plugin_->ask(this, request.as<JsonObjectConst>()) != 0;
    if (!asked_) retry_at_ = epoch + RETRY;
  }

  // A sample at the end of the line, in order of time; the oldest goes when the line is full.
  void add(uint32_t at, float minutes) {
    if (count_ && at < samples_[count_ - 1].at) return;
    if (count_ && at == samples_[count_ - 1].at) {
      samples_[count_ - 1].minutes = minutes;
      return;
    }
    if (count_ == SAMPLES) {
      memmove(samples_, samples_ + 1, sizeof(Sample) * (SAMPLES - 1));
      --count_;
    }
    samples_[count_++] = {at, minutes};
  }

  // The times under the line, once a minute; the line itself moves on with them.
  void paint_times(uint32_t epoch) {
    if (!graph_ || !epoch) return;
    ui::set_text(from_, tessera::clock_text(epoch - static_cast<uint32_t>(hours_) * 3600u));
    ui::set_text(now_, plugin_->text("now"));
    if (epoch / 60 != minute_) {
      minute_ = epoch / 60;
      lv_obj_invalidate(graph_);
    }
  }

  static void draw(lv_event_t *e) {
    auto *tile = static_cast<TravelTile *>(lv_event_get_user_data(e));
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    tile->paint(lv_event_get_layer(e), a);
  }

  // The travel time of the last hours as a line, the usual time dashed across it, a hairline at the bottom.
  void paint(lv_layer_t *layer, const lv_area_t &a) {
    const uint32_t now = tessera::epoch();
    const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (!now || w < 4 || h < 4) return;
    const uint32_t start = now - static_cast<uint32_t>(hours_) * 3600u;

    lv_draw_line_dsc_t base;
    lv_draw_line_dsc_init(&base);
    base.color = ui::color(theme::GRID);
    base.width = std::max(1, ui::px(1));
    base.p1 = {static_cast<lv_value_precise_t>(a.x1), static_cast<lv_value_precise_t>(a.y2)};
    base.p2 = {static_cast<lv_value_precise_t>(a.x2), static_cast<lv_value_precise_t>(a.y2)};
    lv_draw_line(layer, &base);

    // The range: the samples in the window and the usual time, at least four minutes high.
    float lo = INFINITY, hi = -INFINITY;
    int first = 0;
    while (first + 1 < count_ && samples_[first + 1].at <= start) ++first;  // one before the window starts the line
    for (int i = first; i < count_; ++i) {
      lo = std::min(lo, samples_[i].minutes);
      hi = std::max(hi, samples_[i].minutes);
    }
    if (count_ == 0) return;
    if (usual_ > 0) {
      lo = std::min(lo, usual_);
      hi = std::max(hi, usual_);
    }
    if (hi - lo < 4) {
      const float mid = (hi + lo) / 2;
      lo = mid - 2;
      hi = mid + 2;
    }
    const int lw = std::max(2, ui::px(2));
    auto x_of = [&](uint32_t at) {
      const float t = at <= start ? 0.f : static_cast<float>(at - start) / static_cast<float>(now - start);
      return static_cast<lv_value_precise_t>(a.x1 + lw / 2.0f + (w - lw) * std::min(1.f, t));
    };
    auto y_of = [&](float v) {
      return static_cast<lv_value_precise_t>(a.y1 + lw / 2.0f + (h - lw - ui::px(1)) * (1 - (v - lo) / (hi - lo)));
    };

    if (usual_ > 0) {
      lv_draw_line_dsc_t dash;
      lv_draw_line_dsc_init(&dash);
      dash.color = ui::color(theme::SUBTLE);
      dash.width = std::max(1, ui::px(1));
      dash.dash_width = ui::px(4);
      dash.dash_gap = ui::px(4);
      dash.p1 = {static_cast<lv_value_precise_t>(a.x1), y_of(usual_)};
      dash.p2 = {static_cast<lv_value_precise_t>(a.x2), y_of(usual_)};
      lv_draw_line(layer, &dash);
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = ui::tone_color(tone_, theme::ACCENT);
    line.width = lw;
    line.round_start = line.round_end = 1;
    lv_point_precise_t prev = {x_of(samples_[first].at), y_of(samples_[first].minutes)};
    for (int i = first + 1; i <= count_; ++i) {
      // The last sample holds until now.
      const lv_point_precise_t next = i < count_ ? lv_point_precise_t{x_of(samples_[i].at), y_of(samples_[i].minutes)}
                                                 : lv_point_precise_t{x_of(now), prev.y};
      line.p1 = prev;
      line.p2 = next;
      lv_draw_line(layer, &line);
      prev = next;
    }
  }

  WazeTraveltime *plugin_;
  tessera::Text entity_, title_, name_text_, route_;
  char region_[4] = "eu";
  int width_ = 0, height_ = 0, tile_ = -1, graph_h_ = 0, hours_ = 3, number_y_ = 0, info_y_ = 0;
  bool wide_ = false, chosen_ = false, asked_ = false;
  float usual_ = 0, minutes_ = NAN;
  Font face_ = Font::VALUE;
  Tone tone_ = Tone::ACCENT;
  uint32_t retry_at_ = 0, minute_ = 0;
  Sample samples_[SAMPLES];
  int count_ = 0;
  lv_obj_t *name_{}, *delta_{}, *dot_{}, *number_{}, *unit_{}, *info_{}, *graph_{}, *from_{}, *now_{};
};


// ---- The card: every route Waze offers right now, fastest first ----
class RoutesCard : public tessera::Card, public Asker {
 public:
  explicit RoutesCard(WazeTraveltime *plugin) : plugin_(plugin) {}
  ~RoutesCard() override { plugin_->forget(this); }

  void open(const tessera::CardContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    note_ = ui::text(parent_, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_width(note_, width_);
    ui::set_text(note_, plugin_->text("waiting"));

    // A row per route, as many as fit, made now and hidden until the answer fills them: its name and time, a bar as
    // long as the time, the distance and the difference.
    const int title = ui::line_height(Font::TITLE), body = ui::line_height(Font::BODY), bar = ui::px(8);
    const int gap = ui::px(ui::large() ? 6 : 4), step = title + gap + bar + gap + body + ui::px(ui::large() ? 18 : 10);
    fit_ = std::clamp((height_ + step - title - gap - bar - gap - body) / step, 1, ROWS);
    for (int i = 0; i < fit_; ++i) {
      Row &r = row_[i];
      const int y = i * step;
      r.time = ui::label(parent_, Font::TITLE, theme::INK);
      lv_obj_set_pos(r.time, width_, y);
      r.name = ui::label(parent_, Font::TITLE, theme::INK);
      lv_obj_set_pos(r.name, 0, y);
      r.track = ui::block(parent_, theme::TRACK);
      lv_obj_set_size(r.track, width_, bar);
      lv_obj_set_pos(r.track, 0, y + title + gap);
      r.fill = ui::block(parent_, theme::MUTED);
      lv_obj_set_size(r.fill, bar, bar);
      lv_obj_set_pos(r.fill, 0, y + title + gap);
      r.detail = ui::label(parent_, Font::BODY, theme::MUTED);
      lv_obj_set_width(r.detail, width_);
      lv_obj_set_pos(r.detail, 0, y + title + gap + bar + gap);
      show(r, false);
    }

    // The trip of the sensor it was opened for, as its tiles told the plugin; else it waits for the sensor in on_state,
    // with the region of the tile that showed the sensor.
    const Trip *trip = plugin_->trip(c.entity ? c.entity : "");
    if (trip) snprintf(region_, sizeof region_, "%s", trip->region);
    if (trip && *trip->origin && *trip->destination) ask(trip->origin, trip->destination);
  }

  // The tile it was opened from: its sensor's two ends, when the plugin did not know them yet.
  void on_state(JsonObjectConst data) override {
    if (asked_) return;
    const char *origin = data["attributes"]["origin"] | "", *destination = data["attributes"]["destination"] | "";
    if (*origin && *destination) ask(origin, destination);
  }

  void on_theme() override {
    for (int i = 0; i < fit_; ++i) {
      Row &r = row_[i];
      ui::set_color(r.name, theme::INK);
      ui::set_color(r.time, theme::INK);
      ui::set_color(r.detail, theme::MUTED);
      ui::set_fill(r.fill, i == 0 ? Tone::ACCENT : Tone::NORMAL, theme::MUTED);
    }
  }

  // The answer of waze_travel_time.get_travel_times: {"routes": [{"duration", "distance", "name", ...}]}.
  void answer(JsonObjectConst message) override {
    if (filled_) return;
    if (!(message["ok"] | false)) {
      ui::set_text(note_, plugin_->text("failed"));
      return;
    }
    struct Route {
      char name[52];
      float minutes, distance;
    };
    Route routes[ROWS];
    int n = 0;
    for (JsonObjectConst r : message["result"]["routes"].as<JsonArrayConst>()) {
      const float minutes = r["duration"] | NAN;
      if (!std::isfinite(minutes)) continue;
      Route &route = routes[n < ROWS ? n : ROWS - 1];
      // When there are more routes than rows, the slowest of those kept makes room for this one.
      if (n == ROWS) {
        int slowest = 0;
        for (int i = 1; i < ROWS; ++i)
          if (routes[i].minutes > routes[slowest].minutes) slowest = i;
        if (minutes >= routes[slowest].minutes) continue;
        routes[slowest] = routes[ROWS - 1];
      } else {
        ++n;
      }
      snprintf(route.name, sizeof route.name, "%s", r["name"] | "");
      route.minutes = minutes;
      route.distance = r["distance"] | NAN;
    }
    if (n == 0) {
      ui::set_text(note_, plugin_->text("no_routes"));
      return;
    }
    std::sort(routes, routes + n, [](const Route &a, const Route &b) { return a.minutes < b.minutes; });
    lv_obj_add_flag(note_, LV_OBJ_FLAG_HIDDEN);
    filled_ = true;

    const int gap = ui::px(ui::large() ? 6 : 4), bar = ui::px(8);
    const float slowest = routes[n - 1].minutes;
    for (int i = 0; i < std::min(n, fit_); ++i) {
      Row &r = row_[i];
      const Route &route = routes[i];
      const std::string time = whole(route.minutes) + " " + plugin_->text("min");
      const int time_w = ui::text_width(time, Font::TITLE) + ui::px(2);
      lv_obj_set_width(r.time, time_w);
      lv_obj_set_x(r.time, width_ - time_w);
      ui::set_text(r.time, time);
      lv_obj_set_width(r.name, std::max(1, width_ - time_w - gap * 2));
      ui::set_text(r.name, route.name);
      lv_obj_set_width(r.fill, std::max(bar, static_cast<int>(width_ * route.minutes / slowest)));
      ui::set_fill(r.fill, i == 0 ? Tone::ACCENT : Tone::NORMAL, theme::MUTED);
      std::string detail = std::isfinite(route.distance) ? km(plugin_, route.distance) : "";
      const std::string how = i == 0 ? std::string(plugin_->text("fastest"))
                                     : tessera::fill(plugin_->text("slower"), "n",
                                                     whole(route.minutes - routes[0].minutes));
      ui::set_text(r.detail, detail.empty() ? how : detail + " · " + how);
      show(r, true);
    }
  }

 private:
  static constexpr int ROWS = 6;
  struct Row {
    lv_obj_t *name{}, *time{}, *track{}, *fill{}, *detail{};
  };

  static void show(Row &r, bool shown) {
    for (lv_obj_t *part : {r.name, r.time, r.track, r.fill, r.detail}) {
      if (shown) lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
  }

  void ask(const char *origin, const char *destination) {
    asked_ = true;
    JsonDocument request;
    request["ask"] = "call_service:waze_travel_time.get_travel_times";
    request["data"]["origin"] = origin;
    request["data"]["destination"] = destination;
    request["data"]["region"] = region_;
    request["data"]["realtime"] = true;  // live traffic: the action's own default is without
    if (!plugin_->ask(this, request.as<JsonObjectConst>())) ui::set_text(note_, plugin_->text("failed"));
  }

  WazeTraveltime *plugin_;
  lv_obj_t *parent_{}, *note_{};
  int width_ = 0, height_ = 0, fit_ = 0;
  bool asked_ = false, filled_ = false;
  char region_[4] = "eu";
  Row row_[ROWS];
};

// ---- The plugin ----

void WazeTraveltime::setup() {
  add_tile("travel", [this]() { return new TravelTile(this); });
  add_card("routes", [this]() { return new RoutesCard(this); }, true);
  add_bar_item("travel", [this]() { return bar(); });
}

// "23 min" in the top bar, or "23 min +8" when it is slower than the usual time, the car green, amber or red by the
// delay. Nothing while the sensor has no travel time.
tessera::BarItem WazeTraveltime::bar() const {
  tessera::BarItem item;
  if (!minutes_ || !minutes_->has_state() || !std::isfinite(minutes_->state) || minutes_->state < 0) return item;
  item.shown = true;
  item.icon = 0xF010B;  // car
  const float minutes = minutes_->state;
  item.text = tessera::fill(text("bar"), "n", whole(minutes));
  const float usual = usual_ && std::isfinite(usual_->state) ? usual_->state : 0;
  if (usual > 0) {
    const float delay = minutes - usual;
    item.tone = tone_of(delay);
    if (lroundf(delay) >= 1)
      item.text = tessera::fill(tessera::fill(text("bar_late"), "n", whole(minutes)).c_str(), "d", whole(delay));
  }
  return item;
}

bool WazeTraveltime::settings(tessera::SettingsPage &page) {
  page.icon = "\U000F010B";
  page.number(text("usual_time"), 0, 240, 1, text("min"),
              [this]() { return usual_ && std::isfinite(usual_->state) ? static_cast<int>(usual_->state) : 0; },
              [this](int value) {
                if (!usual_) return;
                auto call = usual_->make_call();
                call.set_value(value);
                call.perform();
              });
  return true;
}

uint32_t WazeTraveltime::ask(Asker *asker, JsonObjectConst request) {
  const uint32_t re = tessera::send(this, request);
  if (!re) return 0;
  // The slot of an answer that never came is taken over in turn.
  Waiting *slot = &waiting_[re % 8];
  for (Waiting &w : waiting_)
    if (!w.asker) { slot = &w; break; }
  slot->re = re;
  slot->asker = asker;
  return re;
}

void WazeTraveltime::remember(const char *entity, const char *origin, const char *destination, const char *region) {
  Trip *slot = nullptr;
  for (Trip &t : trips_)
    if (strcmp(t.entity, entity) == 0) { slot = &t; break; }
  if (!slot) {
    slot = &trips_[next_trip_];
    next_trip_ = (next_trip_ + 1) % 4;
    *slot = Trip{};
  }
  snprintf(slot->entity, sizeof slot->entity, "%s", entity);
  if (origin) snprintf(slot->origin, sizeof slot->origin, "%s", origin);
  if (destination) snprintf(slot->destination, sizeof slot->destination, "%s", destination);
  snprintf(slot->region, sizeof slot->region, "%s", region);
}

const Trip *WazeTraveltime::trip(const char *entity) const {
  if (!*entity) return nullptr;
  for (const Trip &t : trips_)
    if (strcmp(t.entity, entity) == 0) return &t;
  return nullptr;
}

void WazeTraveltime::forget(Asker *asker) {
  for (Waiting &w : waiting_)
    if (w.asker == asker) w = Waiting{};
}

void WazeTraveltime::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  for (Waiting &w : waiting_) {
    if (!w.asker || w.re != re) continue;
    Asker *asker = w.asker;
    w = Waiting{};
    asker->answer(message);
    return;
  }
}

}  // namespace esphome::waze_traveltime
