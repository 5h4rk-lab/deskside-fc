// The Sound rows of the Settings screen.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "screens/settings.h"
#include "test_display.h"
#include "theme.h"

namespace {

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

bool visible(lv_obj_t* obj) { return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }
std::string label_text(lv_obj_t* parent) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
    lv_obj_t* c = lv_obj_get_child(parent, static_cast<int32_t>(i));
    if (lv_obj_check_type(c, &lv_label_class) && lv_label_get_text(c)[0] != '\xEF') return lv_label_get_text(c);
  }
  return "";
}

struct Screen {
  NoImages images;
  std::vector<Settings> changes;
  int tests = 0;
  lv_obj_t* obj = nullptr;

  explicit Screen(bool sound = true, const Settings& s = Settings()) {
    init_test_display();
    theme_reset();
    SettingsCallbacks cb;
    if (sound) cb.onTestSound = [this] { ++tests; };
    cb.onChanged = [this](const Settings& n) { changes.push_back(n); };
    obj = settings_create(lv_screen_active(), s, images, cb, 1784116800);
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  ~Screen() { deinit_test_display(); }
  const SettingsParts* parts() const { return settings_parts(obj); }
  void click(lv_obj_t* o) { lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr); }
  void toggle(lv_obj_t* sw) {
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
  }
};

}  // namespace

TEST_CASE("sound rows: absent without a speaker") {
  Screen s(false);
  const SettingsParts* p = s.parts();
  CHECK(p->testSound == nullptr);
  CHECK(p->soundRows[0] == nullptr);
  CHECK(p->volumeValue == nullptr);
  CHECK(p->soundNightSwitch == nullptr);
}

TEST_CASE("sound rows: the three choices, the helper line and the defaults") {
  Screen s;
  const SettingsParts* p = s.parts();
  REQUIRE(p->soundRows[2] != nullptr);
  CHECK(label_text(p->soundRows[0]) == "Off");
  CHECK(label_text(p->soundRows[1]) == "Goals only");
  CHECK(label_text(p->soundRows[2]) == "Goals and match alerts");
  CHECK(std::string(lv_label_get_text(p->soundHelp)) == "The speaker only plays for your team.");
  CHECK_FALSE(visible(p->soundTicks[0]));
  CHECK(visible(p->soundTicks[1]));
  CHECK_FALSE(visible(p->soundTicks[2]));
  CHECK(std::string(lv_label_get_text(p->volumeValue)) == "40%");
  CHECK(lv_obj_has_state(p->soundNightSwitch, LV_STATE_CHECKED));
  CHECK(label_text(p->testSound) == "Test sound");
  CHECK(label_text(lv_obj_get_parent(p->soundNightSwitch)) == "Mute at night");
}

TEST_CASE("sound rows: choosing a mode moves the tick and reports one change") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.click(p->soundRows[2]);
  REQUIRE(s.changes.size() == 1);
  CHECK(s.changes[0].soundMode == SoundMode::GoalsAndAlerts);
  CHECK(visible(p->soundTicks[2]));
  CHECK_FALSE(visible(p->soundTicks[1]));
  s.click(p->soundRows[0]);
  CHECK(s.changes.back().soundMode == SoundMode::Off);
  CHECK(visible(p->soundTicks[0]));
  s.click(p->soundRows[0]);  // the same again: nothing new
  CHECK(s.changes.size() == 2);
}

TEST_CASE("sound rows: the volume steps through 20 40 60 80 100 and stops at the ends") {
  Screen s;
  const SettingsParts* p = s.parts();
  const int expected[] = {60, 80, 100};
  for (int v : expected) {
    s.click(p->volumePlus);
    CHECK(s.changes.back().soundVolume == v);
    CHECK(std::string(lv_label_get_text(p->volumeValue)) == std::to_string(v) + "%");
  }
  const size_t n = s.changes.size();
  s.click(p->volumePlus);  // at the top: nothing
  CHECK(s.changes.size() == n);
  for (int v : {80, 60, 40, 20}) {
    s.click(p->volumeMinus);
    CHECK(s.changes.back().soundVolume == v);
  }
  const size_t m = s.changes.size();
  s.click(p->volumeMinus);
  CHECK(s.changes.size() == m);
  CHECK(std::string(lv_label_get_text(p->volumeValue)) == "20%");
}

TEST_CASE("sound rows: the mute switch and the Test sound button") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.toggle(p->soundNightSwitch);
  REQUIRE(s.changes.size() == 1);
  CHECK_FALSE(s.changes[0].soundMuteAtNight);
  s.toggle(p->soundNightSwitch);
  CHECK(s.changes.back().soundMuteAtNight);
  s.click(p->testSound);
  s.click(p->testSound);
  CHECK(s.tests == 2);
  CHECK(s.changes.size() == 2);  // the button changes no setting
}

TEST_CASE("sound rows: shows the settings it is given, and a refresh follows them") {
  Settings st;
  st.soundMode = SoundMode::Off;
  st.soundVolume = 100;
  st.soundMuteAtNight = false;
  Screen s(true, st);
  const SettingsParts* p = s.parts();
  CHECK(visible(p->soundTicks[0]));
  CHECK(std::string(lv_label_get_text(p->volumeValue)) == "100%");
  CHECK_FALSE(lv_obj_has_state(p->soundNightSwitch, LV_STATE_CHECKED));
  st.soundMode = SoundMode::GoalsAndAlerts;
  st.soundVolume = 20;
  settings_refresh(s.obj, st, 1784116800);
  CHECK(visible(p->soundTicks[2]));
  CHECK_FALSE(visible(p->soundTicks[0]));
  CHECK(std::string(lv_label_get_text(p->volumeValue)) == "20%");
}

TEST_CASE("sound rows: every control fits the 240 px screen and the list scrolls to the last one") {
  Screen s;
  const SettingsParts* p = s.parts();
  lv_obj_t* controls[] = {p->soundRows[0], p->soundRows[1], p->soundRows[2], p->volumeMinus,
                          p->volumePlus,   p->testSound,    p->soundNightSwitch};
  for (lv_obj_t* o : controls) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    CHECK(a.x1 >= 0);
    CHECK(a.x2 < 240);
    CHECK(lv_obj_get_width(o) >= 22);
    CHECK(lv_obj_get_height(o) >= 22);
  }
  // Scrolling the list reaches each of them.
  for (lv_obj_t* o : controls) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_area_t list;
    lv_obj_get_coords(p->list, &list);
    lv_obj_scroll_to_y(p->list, lv_obj_get_scroll_y(p->list) + (a.y1 - list.y1) - 20, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(o, &a);
    CHECK(a.y1 >= list.y1 - 1);
    CHECK(a.y2 <= list.y2 + 1);
  }
}

TEST_CASE("sound rows: sit between Reduced motion and the rest, in order") {
  Screen s;
  const SettingsParts* p = s.parts();
  lv_obj_t* motion = lv_obj_get_parent(p->motionHelp);
  lv_obj_t* sound = lv_obj_get_parent(p->soundRows[0]);
  lv_obj_t* volume = lv_obj_get_parent(p->testSound);
  lv_obj_t* quiet = lv_obj_get_parent(lv_obj_get_parent(p->soundNightSwitch));
  CHECK(lv_obj_get_index(sound) == lv_obj_get_index(motion) + 1);
  CHECK(lv_obj_get_index(volume) == lv_obj_get_index(sound) + 1);
  CHECK(lv_obj_get_index(quiet) == lv_obj_get_index(volume) + 1);
}
