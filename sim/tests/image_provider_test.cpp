#include <doctest/doctest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "image_provider.h"
#include "mock_image_provider.h"
#include "test_display.h"
#include "test_images.h"

namespace fs = std::filesystem;

namespace {

// Fresh directory under the system temp dir, removed on destruction.
struct TempDir {
  fs::path path;
  TempDir() {
    static std::atomic<int> counter{0};
    path = fs::temp_directory_path() /
           ("soccer-image-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void write(const std::string& name, const uint8_t* data, size_t n) const {
    std::ofstream out(path / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
  }
  void write_text(const std::string& name, const std::string& s) const {
    write(name, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  std::string str() const { return path.string(); }
};

constexpr const char* kCrest57 =
    "/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F57.png&w=48&h=48";
constexpr const char* kCrest61 =
    "/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F61.png&w=48&h=48";

// Holds the callback so the test decides when the "download" finishes.
class DeferredProvider : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override {
    pending.push_back(std::move(done));
  }
  void fire(size_t i, ImagePtr img) { pending.at(i)(std::move(img)); }
  std::vector<std::function<void(ImagePtr)>> pending;
};

ImagePtr make_image(int w, int h, uint8_t fill) {
  auto* dsc = new lv_image_dsc_t();
  dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
  dsc->header.w = w;
  dsc->header.h = h;
  dsc->header.stride = w * 4;
  dsc->data_size = static_cast<uint32_t>(w * h * 4);
  auto* px = new uint8_t[dsc->data_size];
  for (uint32_t i = 0; i < dsc->data_size; ++i) px[i] = fill;
  dsc->data = px;
  return ImagePtr(dsc, [](const lv_image_dsc_t* d) {
    delete[] d->data;
    delete d;
  });
}

}  // namespace

TEST_CASE("mock_asset_name maps the helper image paths to asset file names") {
  CHECK(mock_asset_name(kCrest57) == "crest-57.png");
  CHECK(mock_asset_name("/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F367.png&w=48&h=48") ==
        "espn-367.png");
  CHECK(mock_asset_name("/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fphoto%2F2026%2F1019%2Fr1733102_1296x729_16-9.jpg&fit=cover&w=240&h=135") ==
        "img-r1733102_1296x729_16-9.jpg");
  CHECK(mock_asset_name("/api/img?w=48&u=https%3A%2F%2Fcrests.football-data.org%2F5.png") ==
        "crest-5.png");  // parameter order does not matter
  CHECK(mock_asset_name("/api/cutout?name=Kai%20Havertz&w=40&h=40") == "cutout-Kai_Havertz.png");
}

TEST_CASE("mock_asset_name percent-decodes UTF-8 and maps each non-alphanumeric character to _") {
  CHECK(mock_asset_name("/api/cutout?name=Mathys%20Tel") == "cutout-Mathys_Tel.png");
  // One character is one underscore, however many UTF-8 bytes it takes.
  CHECK(mock_asset_name("/api/cutout?name=%C3%98degaard") == "cutout-_degaard.png");
  CHECK(mock_asset_name("/api/cutout?name=N%27Golo%20Kant%C3%A9") == "cutout-N_Golo_Kant_.png");
  // Decoded separators never reach the file name as path separators.
  CHECK(mock_asset_name("/api/cutout?name=..%2F..%2Fetc%2Fpasswd") == "cutout-______etc_passwd.png");
  CHECK(mock_asset_name("/api/img?u=https%3A%2F%2Fexample.com%2Fa%2F..%2Fb.jpg") == "img-b.jpg");
}

TEST_CASE("mock_asset_name returns an empty string for unknown shapes") {
  CHECK(mock_asset_name("") == "");
  CHECK(mock_asset_name("garbage") == "");
  CHECK(mock_asset_name("/api/other?u=x") == "");
  CHECK(mock_asset_name("/api/img") == "");
  CHECK(mock_asset_name("/api/img?w=48") == "");
  CHECK(mock_asset_name("/api/img?u=") == "");
  CHECK(mock_asset_name("/api/img?u=https%3A%2F%2Fexample.com%2F") == "");
  CHECK(mock_asset_name("/api/cutout?w=40") == "");
  CHECK(mock_asset_name("/api/cutout?name=") == "");
}

TEST_CASE("MockImageProvider decodes a PNG and caches it") {
  init_test_display();
  {
    TempDir dir;
    dir.write("crest-57.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
    MockImageProvider provider(dir.str());

    int calls = 0;
    ImagePtr first;
    provider.request(kCrest57, [&](ImagePtr img) {
      ++calls;
      first = img;
    });
    CHECK(calls == 1);  // synchronous
    REQUIRE(first);
    CHECK(first->header.w == 4);
    CHECK(first->header.h == 4);
    CHECK(first->header.cf == LV_COLOR_FORMAT_ARGB8888);  // alpha survives
    REQUIRE(first->data != nullptr);
    CHECK(first->data_size >= 4u * 4u * 4u);

    // Pixel 1 of the source is (r=16, g=239, b=128, a=255). ARGB8888 is B,G,R,A in memory.
    const uint8_t* px = first->data + 4;
    CHECK(px[0] == 128);
    CHECK(px[1] == 239);
    CHECK(px[2] == 16);
    CHECK(px[3] == 255);
    // Pixel 0 has half alpha: alpha is preserved, not flattened.
    CHECK(first->data[3] < 255);

    ImagePtr second;
    provider.request(kCrest57, [&](ImagePtr img) { second = img; });
    CHECK(second.get() == first.get());  // same cached pointer

    // The file is gone, the cache still serves it.
    fs::remove(dir.path / "crest-57.png");
    ImagePtr third;
    provider.request(kCrest57, [&](ImagePtr img) { third = img; });
    CHECK(third.get() == first.get());
  }
  deinit_test_display();
}

TEST_CASE("MockImageProvider decodes a JPEG") {
  init_test_display();
  {
    TempDir dir;
    dir.write("img-photo.jpg", test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8));
    MockImageProvider provider(dir.str());
    ImagePtr img;
    provider.request("/api/img?u=https%3A%2F%2Fexample.com%2Fphotos%2Fphoto.jpg&fit=cover&w=240&h=135",
                     [&](ImagePtr i) { img = i; });
    REQUIRE(img);
    CHECK(img->header.w == 16);
    CHECK(img->header.h == 8);
    CHECK(img->header.cf == LV_COLOR_FORMAT_ARGB8888);
    // The source is a flat red; JPEG is lossy so allow some error.
    const uint8_t* px = img->data;  // B,G,R,A
    CHECK(px[2] > 170);
    CHECK(px[1] < 70);
    CHECK(px[0] < 70);
    CHECK(px[3] == 255);
  }
  deinit_test_display();
}

TEST_CASE("MockImageProvider delivers nullptr for missing, unknown or undecodable files") {
  init_test_display();
  {
    TempDir dir;
    dir.write_text("crest-61.png", "this is not a png");
    dir.write("crest-64.png", test_images::kPng4x4, 20);  // truncated PNG
    MockImageProvider provider(dir.str());

    auto expect_null = [&](const std::string& path) {
      int calls = 0;
      ImagePtr got = make_image(1, 1, 0);  // must be overwritten with null
      provider.request(path, [&](ImagePtr img) {
        ++calls;
        got = img;
      });
      CHECK(calls == 1);
      CHECK(got == nullptr);
    };
    expect_null(kCrest57);  // no such file
    expect_null(kCrest61);  // garbage content
    expect_null("/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F64.png");  // truncated
    expect_null("not-a-helper-path");
    expect_null("");
  }
  deinit_test_display();
}

TEST_CASE("MockImageProvider evicts under a tiny budget without invalidating held images") {
  init_test_display();
  {
    TempDir dir;
    dir.write("crest-57.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
    dir.write("crest-61.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
    // One 4x4 ARGB8888 image is 64 bytes; this budget holds exactly one.
    MockImageProvider provider(dir.str(), 100);

    ImagePtr a;
    provider.request(kCrest57, [&](ImagePtr i) { a = i; });
    REQUIRE(a);
    const lv_image_dsc_t* aRaw = a.get();

    ImagePtr b;
    provider.request(kCrest61, [&](ImagePtr i) { b = i; });  // evicts 57
    REQUIRE(b);
    CHECK(b.get() != aRaw);

    // 'a' is no longer cached but is still fully readable.
    CHECK(a->header.w == 4);
    CHECK(a->header.h == 4);
    REQUIRE(a->data != nullptr);
    CHECK(a->data[4 + 1] == 239);

    // Requesting it again decodes a fresh copy.
    ImagePtr a2;
    provider.request(kCrest57, [&](ImagePtr i) { a2 = i; });
    REQUIRE(a2);
    CHECK(a2.get() != aRaw);
    CHECK(a2->header.w == 4);
  }
  deinit_test_display();
}

TEST_CASE("MockImageProvider does not cache images larger than its budget") {
  init_test_display();
  {
    TempDir dir;
    dir.write("crest-57.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
    MockImageProvider provider(dir.str(), 10);
    ImagePtr a;
    provider.request(kCrest57, [&](ImagePtr i) { a = i; });
    REQUIRE(a);  // still delivered
    CHECK(a->header.w == 4);
    ImagePtr b;
    provider.request(kCrest57, [&](ImagePtr i) { b = i; });
    REQUIRE(b);
    CHECK(b.get() != a.get());
  }
  deinit_test_display();
}

TEST_CASE("image_load_into sets the widget source and keeps the image alive past the provider") {
  init_test_display();
  {
    TempDir dir;
    dir.write("crest-57.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
    lv_obj_t* img = lv_image_create(lv_screen_active());
    {
      MockImageProvider provider(dir.str(), 100);
      image_load_into(img, provider, kCrest57);
      const void* src = lv_image_get_src(img);
      REQUIRE(src != nullptr);
      const auto* dsc = static_cast<const lv_image_dsc_t*>(src);
      CHECK(dsc->header.w == 4);

      // Push the entry out of the cache, then destroy the provider.
      dir.write("crest-61.png", test_images::kPng4x4, sizeof(test_images::kPng4x4));
      provider.request(kCrest61, [](ImagePtr) {});
    }  // provider destroyed

    const auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(img));
    REQUIRE(dsc != nullptr);
    CHECK(dsc->header.w == 4);
    CHECK(dsc->data[4 + 1] == 239);  // memory still valid
    test_advance(50);                // renders the image without crashing

    lv_obj_delete(img);  // releases the image exactly once
    test_advance(10);
  }
  deinit_test_display();
}

TEST_CASE("image_load_into leaves the placeholder when the image is unavailable") {
  init_test_display();
  {
    TempDir dir;
    MockImageProvider provider(dir.str());
    lv_obj_t* img = lv_image_create(lv_screen_active());
    image_load_into(img, provider, kCrest57);
    CHECK(lv_image_get_src(img) == nullptr);
    lv_obj_delete(img);
  }
  deinit_test_display();
}

TEST_CASE("image_load_into ignores a late callback after the widget was deleted") {
  init_test_display();
  {
    DeferredProvider provider;
    lv_obj_t* doomed = lv_image_create(lv_screen_active());
    lv_obj_t* survivor = lv_image_create(lv_screen_active());
    image_load_into(doomed, provider, "/doomed");
    image_load_into(survivor, provider, "/survivor");
    REQUIRE(provider.pending.size() == 2);

    lv_obj_delete(doomed);
    test_advance(10);

    ImagePtr late = make_image(8, 8, 0x11);
    ImagePtr good = make_image(6, 6, 0x22);
    provider.fire(0, late);  // widget is gone: must do nothing, not crash
    provider.fire(1, good);

    const auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(survivor));
    REQUIRE(dsc != nullptr);
    CHECK(dsc == good.get());
    CHECK(dsc->header.w == 6);
    CHECK(late.use_count() == 1);  // the dead widget's callback did not retain it

    test_advance(50);
    lv_obj_delete(survivor);
  }
  deinit_test_display();
}

TEST_CASE("image_load_into holds shared ownership for exactly the widget's lifetime") {
  init_test_display();
  {
    DeferredProvider provider;
    lv_obj_t* img = lv_image_create(lv_screen_active());
    image_load_into(img, provider, "/x");
    ImagePtr image = make_image(4, 4, 0x33);
    provider.fire(0, image);
    CHECK(image.use_count() == 2);  // caller + widget holder
    lv_obj_delete(img);
    CHECK(image.use_count() == 1);
  }
  deinit_test_display();
}

TEST_CASE("image_load_into called again replaces the previous request") {
  init_test_display();
  {
    DeferredProvider provider;
    lv_obj_t* img = lv_image_create(lv_screen_active());
    image_load_into(img, provider, "/first");
    ImagePtr first = make_image(4, 4, 0x01);
    provider.fire(0, first);
    CHECK(lv_image_get_src(img) == first.get());

    image_load_into(img, provider, "/second");  // a second request for the same widget
    image_load_into(img, provider, "/third");
    REQUIRE(provider.pending.size() == 3);
    // While the replacement loads, the shown image stays valid.
    CHECK(lv_image_get_src(img) == first.get());
    CHECK(first.use_count() == 2);

    ImagePtr stale = make_image(5, 5, 0x02);
    ImagePtr fresh = make_image(6, 6, 0x03);
    provider.fire(1, stale);  // superseded: ignored
    CHECK(lv_image_get_src(img) == first.get());
    CHECK(stale.use_count() == 1);
    provider.fire(2, fresh);
    CHECK(lv_image_get_src(img) == fresh.get());
    CHECK(first.use_count() == 1);  // released once replaced

    lv_obj_delete(img);
    CHECK(fresh.use_count() == 1);
    // Callbacks firing after deletion are harmless too.
    provider.fire(1, stale);
    provider.fire(2, fresh);
  }
  deinit_test_display();
}
