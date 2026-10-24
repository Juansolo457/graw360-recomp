// GRAW 360 - a scripted virtual controller. See autopilot.h.

#include "autopilot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_STRING(graw_autopilot, "", "GRAW",
                      "A script of controller input to play from start-up (see src/autopilot.h); "
                      "adds a virtual controller that needs no window focus")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace graw::autopilot {

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr DeviceId kDevice = static_cast<DeviceId>(0x47524157);  // "GRAW"

struct Event {
  double start = 0, end = 0;
  uint16_t buttons = 0;
  bool stick = false, trigger = false;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

const std::map<std::string, uint16_t> kButtons = {
    {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},
    {"LB", 0x0100},    {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},
    {"X", 0x4000},     {"Y", 0x8000}};

int16_t Axis(float v) {
  return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}
uint8_t Trigger(float v) {
  return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

bool Parse(const std::string& path, std::vector<Event>& events) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream words(line);
    Event e;
    std::string what;
    if (!(words >> e.start >> what)) continue;
    double hold = 0.15;
    std::transform(what.begin(), what.end(), what.begin(), ::toupper);
    if (what == "STICK") {
      e.stick = true;
      words >> e.lx >> e.ly >> e.rx >> e.ry;
    } else if (what == "TRIGGER") {
      e.trigger = true;
      words >> e.lt >> e.rt;
    } else {
      std::stringstream names(what);
      std::string name;
      while (std::getline(names, name, '+')) {
        auto it = kButtons.find(name);
        if (it == kButtons.end()) {
          REXLOG_ERROR("GRAW autopilot: line {}: unknown button '{}'", number, name);
          return false;
        }
        e.buttons |= it->second;
      }
    }
    std::string word;
    if (words >> word) {
      std::transform(word.begin(), word.end(), word.begin(), ::tolower);
      if (word == "hold") words >> hold;
    }
    e.end = e.start + hold;
    events.push_back(e);
  }
  return true;
}

class AutopilotDriver final : public rex::input::InputDriver {
 public:
  explicit AutopilotDriver(std::vector<Event> events)
      : InputDriver(nullptr, 0), events_(std::move(events)),
        start_(std::chrono::steady_clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kDevice;
    info.name = "GRAW autopilot";
    info.guid = "graw-autopilot";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    for (const Event& e : events_) {
      if (t < e.start || t >= e.end) continue;
      buttons |= e.buttons;
      if (e.stick) { lx = e.lx; ly = e.ly; rx = e.rx; ry = e.ry; }
      if (e.trigger) { lt = e.lt; rt = e.rt; }
    }
    std::memset(out, 0, sizeof(*out));
    out->packet_number = ++packet_;
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = Trigger(lt);
    out->gamepad.right_trigger = Trigger(rt);
    out->gamepad.thumb_lx = Axis(lx);
    out->gamepad.thumb_ly = Axis(ly);
    out->gamepad.thumb_rx = Axis(rx);
    out->gamepad.thumb_ry = Axis(ry);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out->gamepad.buttons = 0xFFFF;
      out->gamepad.left_trigger = 0xFF;
      out->gamepad.right_trigger = 0xFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::vector<Event> events_;
  std::chrono::steady_clock::time_point start_;
  std::atomic<uint32_t> packet_{0};
};

}  // namespace

std::unique_ptr<rex::input::InputDriver> Create() {
  const std::string path = REXCVAR_GET(graw_autopilot);
  if (path.empty()) return nullptr;
  std::vector<Event> events;
  if (!Parse(path, events)) {
    REXLOG_ERROR("GRAW autopilot: can't read script {}", path);
    return nullptr;
  }
  REXLOG_INFO("GRAW autopilot: {} events from {}", events.size(), path);
  auto driver = std::make_unique<AutopilotDriver>(std::move(events));
  driver->Setup();
  return driver;
}

}  // namespace graw::autopilot
