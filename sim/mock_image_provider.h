// ImageProvider for the simulator: serves images from a local asset directory
// (sim/assets, filled by `node sim/assets/fetch.mjs`). Callbacks run
// synchronously. Decoded images are cached in a byte-budget LRU.
#pragma once

#include <cstddef>
#include <string>

#include "image_provider.h"
#include "lru_bytes.h"

// Maps a helper-relative image path to the local asset file name, or "" for
// unknown shapes. Pure. `w`, `h` and `fit` are ignored.
//   /api/img?u=<crests.football-data.org/ID.png>                  -> crest-ID.png
//   /api/img?u=<a.espncdn.com/i/teamlogos/soccer/500/ID.png>      -> espn-ID.png
//   /api/img?u=<any other url>                                    -> img-<basename of the URL path>
//   /api/cutout?name=<n>                                          -> cutout-<n with each non-[A-Za-z0-9] character as '_'>.png
// "Character" means one UTF-8 code point, so "Ødegaard" gives "_degaard".
std::string mock_asset_name(const std::string& path);

class MockImageProvider : public ImageProvider {
 public:
  static constexpr size_t kDefaultBudget = 2 * 1024 * 1024;

  explicit MockImageProvider(std::string assetDir, size_t cacheBudget = kDefaultBudget);

  // Needs LVGL initialised (uses its PNG decoder). Delivers nullptr when the
  // file is missing or cannot be decoded.
  void request(const std::string& path, std::function<void(ImagePtr)> done) override;

 private:
  ImagePtr load(const std::string& name) const;

  std::string dir_;
  LruBytes<ImagePtr> cache_;
};
