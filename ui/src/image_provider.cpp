#include "image_provider.h"

namespace {

// Per-widget state, owned by the widget's LV_EVENT_DELETE handler.
struct Holder {
  ImagePtr image;                  // keeps the displayed image alive
  std::shared_ptr<bool> liveness;  // cleared when the request is superseded or the widget dies
};

void on_delete(lv_event_t* e) {
  auto* holder = static_cast<Holder*>(lv_event_get_user_data(e));
  *holder->liveness = false;
  delete holder;
}

Holder* find_holder(lv_obj_t* obj) {
  const uint32_t n = lv_obj_get_event_count(obj);
  for (uint32_t i = 0; i < n; ++i) {
    lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
    if (lv_event_dsc_get_cb(dsc) == on_delete) {
      return static_cast<Holder*>(lv_event_dsc_get_user_data(dsc));
    }
  }
  return nullptr;
}

}  // namespace

void image_load_into(lv_obj_t* img, ImageProvider& provider, const std::string& path) {
  if (img == nullptr) return;

  Holder* holder = find_holder(img);
  if (holder != nullptr) {
    *holder->liveness = false;  // supersede the previous request
  } else {
    holder = new Holder();
    lv_obj_add_event_cb(img, on_delete, LV_EVENT_DELETE, holder);
  }
  holder->liveness = std::make_shared<bool>(true);

  // The token outlives the holder; while it is true both the widget and the
  // holder exist (on_delete clears it before freeing either).
  std::shared_ptr<bool> liveness = holder->liveness;
  provider.request(path, [img, holder, liveness](ImagePtr image) {
    if (!*liveness || !image) return;
    holder->image = image;
    lv_image_set_src(img, holder->image.get());
  });
}
