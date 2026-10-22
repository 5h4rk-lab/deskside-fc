#include <doctest/doctest.h>

#include <functional>
#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "http_image_provider.h"
#include "image_decoder.h"
#include "image_provider.h"
#include "test_display.h"

namespace {

constexpr const char* kBase = "https://helper.example";
constexpr const char* kToken = "tok-123";
constexpr const char* kCrest57 = "/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F57.png&w=48&h=48";
constexpr const char* kCrest61 = "/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F61.png&w=48&h=48";
constexpr const char* kCutout = "/api/cutout?name=Kai%20Havertz&w=40&h=40";

// Bodies starting with "OK" decode to a 4x4 ARGB image (64 bytes); anything
// else is undecodable.
class FakeDecoder : public ImageDecoder {
 public:
  ImagePtr decode(const std::string& bytes) override {
    ++calls;
    if (bytes.compare(0, 2, "OK") != 0) return nullptr;
    auto* dsc = new lv_image_dsc_t();
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = 4;
    dsc->header.h = 4;
    dsc->header.stride = 16;
    dsc->data_size = 64;
    auto* px = new uint8_t[64];
    for (int i = 0; i < 64; ++i) px[i] = static_cast<uint8_t>(0x40 + i);
    dsc->data = px;
    return ImagePtr(dsc, [](const lv_image_dsc_t* d) {
      delete[] d->data;
      delete d;
    });
  }
  int calls = 0;
};

// Answers from inside get(), which the transport contract forbids but the
// provider must survive.
class SyncTransport : public HttpTransport {
 public:
  void get(const std::string& url, const std::string&, int, std::function<void(HttpResult)> done) override {
    urls.push_back(url);
    HttpResult r;
    r.status = 200;
    r.body = "OK";
    done(std::move(r));
  }
  std::vector<std::string> urls;
};

struct Rig {
  FakeTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider;
  explicit Rig(size_t budget = HttpImageProvider::kDefaultCacheBytes)
      : provider(transport, decoder, kBase, kToken, budget) {
    provider.tick(0);
  }
};

std::string path_n(int n) { return "/api/img?u=p" + std::to_string(n) + "&w=1&h=1"; }

struct Got {
  int calls = 0;
  ImagePtr image;
  std::function<void(ImagePtr)> cb() {
    return [this](ImagePtr i) {
      ++calls;
      image = std::move(i);
    };
  }
};

}  // namespace

TEST_CASE("http images: a miss is fetched, decoded and cached; the next request is synchronous") {
  Rig rig;
  Got first;
  rig.provider.request(kCrest57, first.cb());
  CHECK(first.calls == 0);  // asynchronous
  REQUIRE(rig.transport.requests.size() == 1);
  CHECK(rig.provider.pending() == 1);

  CHECK(rig.transport.deliver_at(0, 200, "OK-bytes"));
  CHECK(first.calls == 1);
  REQUIRE(first.image);
  CHECK(first.image->header.w == 4);
  CHECK(rig.decoder.calls == 1);
  CHECK(rig.provider.pending() == 0);
  CHECK(rig.provider.cached_bytes() == 64);

  Got second;
  rig.provider.request(kCrest57, second.cb());
  CHECK(second.calls == 1);  // before request() returned
  CHECK(second.image.get() == first.image.get());
  CHECK(rig.transport.requests.size() == 1);  // no second download
  CHECK(rig.decoder.calls == 1);
}

TEST_CASE("http images: the URL, token and timeout reach the transport") {
  FakeTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider(transport, decoder, "https://helper.example///", kToken);
  Got got;
  provider.request(kCutout, got.cb());
  REQUIRE(transport.requests.size() == 1);
  CHECK(transport.requests[0].url == std::string("https://helper.example") + kCutout);
  CHECK(transport.requests[0].token == kToken);
  CHECK(transport.requests[0].timeoutMs == 10000);
  CHECK(HttpImageProvider::kTimeoutMs == 10000);
}

TEST_CASE("http images: callers asking for a path in flight share one request and one image") {
  Rig rig;
  Got a, b, c;
  rig.provider.request(kCrest57, a.cb());
  rig.provider.request(kCrest57, b.cb());
  rig.provider.request(kCrest57, c.cb());
  CHECK(rig.transport.requests.size() == 1);
  CHECK(rig.provider.pending() == 1);

  rig.transport.deliver_at(0, 200, "OK");
  CHECK(a.calls == 1);
  CHECK(b.calls == 1);
  CHECK(c.calls == 1);
  REQUIRE(a.image);
  CHECK(a.image.get() == b.image.get());
  CHECK(a.image.get() == c.image.get());
  CHECK(rig.decoder.calls == 1);
}

TEST_CASE("http images: a path still queued also takes waiters instead of a second request") {
  Rig rig;
  for (int i = 0; i < 2; ++i) rig.provider.request(path_n(i), [](ImagePtr) {});
  Got a, b;
  rig.provider.request(kCrest57, a.cb());  // queued behind the two in flight
  rig.provider.request(kCrest57, b.cb());
  CHECK(rig.transport.requests.size() == 2);
  CHECK(rig.provider.pending() == 3);
  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(rig.transport.requests.size() == 3);
  rig.transport.deliver_at(1, 200, "OK");  // the 57 request (the other one in flight is index 0)
  CHECK(a.calls == 1);
  CHECK(b.calls == 1);
  CHECK(a.image.get() == b.image.get());
  CHECK(a.image != nullptr);
}

TEST_CASE("http images: paths that are not the helper's image endpoints are refused synchronously") {
  Rig rig;
  const std::vector<std::string> bad = {
      "",
      "garbage",
      "/api/matches?leagues=eng.1",
      "/api/img",  // no query
      "api/img?u=x",
      " /api/img?u=x",
      "http://evil.example/api/img?u=x",
      "https://helper.example/api/img?u=x",
      "//evil.example/api/img?u=x",
      "/api/img?u=http://evil.example/a.png",
      "/api/img?u=ftp://x",
      "/api/img?u=x&w=1/../../etc/passwd",
      "/api/img?u=..",
      "/api/cutout?name=a..b",
      "/api/img?u=a b",
      "/api/img?u=a\tb",
      "/api/img?u=a\nb",
      "/api/img?u=a\r\nx-token: z",
      "/api/img?u=a\x01" "b",
      "/api/img?u=a\x7f" "b",
      std::string("/api/img?u=a\0b", 14),
      "/api/img?u=" + std::string(2000, 'a'),
  };
  for (const std::string& p : bad) {
    int calls = 0;
    ImagePtr result = std::make_shared<lv_image_dsc_t>();  // must be replaced by null
    rig.provider.request(p, [&](ImagePtr i) {
      ++calls;
      result = i;
    });
    INFO("path: ", p.substr(0, 40));
    CHECK(calls == 1);  // synchronous
    CHECK(result == nullptr);
  }
  CHECK(rig.transport.requests.empty());
  CHECK(rig.provider.pending() == 0);

  // Exactly at the length limit is allowed; one more is not.
  const std::string prefix = "/api/img?u=";
  const std::string atLimit = prefix + std::string(1024 - prefix.size(), 'a');
  const std::string overLimit = atLimit + "a";
  CHECK(HttpImageProvider::is_allowed_path(atLimit));
  CHECK_FALSE(HttpImageProvider::is_allowed_path(overLimit));
  CHECK(HttpImageProvider::is_allowed_path(kCrest57));
  CHECK(HttpImageProvider::is_allowed_path(kCutout));
  CHECK(HttpImageProvider::is_allowed_path("/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fphoto%2Fx.jpg&fit=cover&w=240&h=135"));
}

TEST_CASE("http images: at most two requests are outstanding, started first in first out") {
  Rig rig;
  std::vector<Got> got(5);
  for (int i = 0; i < 5; ++i) rig.provider.request(path_n(i), got[i].cb());
  REQUIRE(rig.transport.requests.size() == 2);
  CHECK(rig.transport.requests[0].url == std::string(kBase) + path_n(0));
  CHECK(rig.transport.requests[1].url == std::string(kBase) + path_n(1));
  CHECK(rig.transport.pending() == 2);
  CHECK(rig.provider.pending() == 5);
  for (const auto& r : rig.transport.requests) CHECK(r.token == kToken);

  rig.transport.deliver_at(1, 200, "OK");  // finishing any one frees a slot
  REQUIRE(rig.transport.requests.size() == 3);
  CHECK(rig.transport.requests[2].url == std::string(kBase) + path_n(2));
  CHECK(rig.transport.pending() == 2);

  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(rig.transport.requests.size() == 4);
  CHECK(rig.transport.requests[3].url == std::string(kBase) + path_n(3));

  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(rig.transport.requests.size() == 5);
  CHECK(rig.transport.requests[4].url == std::string(kBase) + path_n(4));
  CHECK(rig.transport.pending() == 2);

  while (rig.transport.pending() > 0) rig.transport.deliver_at(0, 200, "OK");
  for (const Got& g : got) {
    CHECK(g.calls == 1);
    CHECK(g.image != nullptr);
  }
  CHECK(rig.provider.pending() == 0);
}

TEST_CASE("http images: when 64 requests wait, the oldest waiting one is answered with nullptr") {
  Rig rig;
  constexpr int kTotal = 2 + 64 + 3;  // two in flight, a full queue, three more
  std::vector<Got> got(kTotal);
  for (int i = 0; i < kTotal; ++i) rig.provider.request(path_n(i), got[i].cb());
  CHECK(rig.transport.requests.size() == 2);

  // Paths 2, 3 and 4 were the oldest waiting ones.
  for (int i = 0; i < kTotal; ++i) {
    INFO("path ", i);
    if (i >= 2 && i <= 4) {
      CHECK(got[i].calls == 1);
      CHECK(got[i].image == nullptr);
    } else {
      CHECK(got[i].calls == 0);
    }
  }
  CHECK(rig.provider.pending() == 2 + 64);

  // The dropped ones were never attempted, so they are not negative-cached.
  Got again;
  rig.provider.request(path_n(2), again.cb());
  CHECK(again.calls == 0);
  CHECK(rig.provider.pending() == 2 + 64);  // 64 waiting again (the oldest, path 5, was dropped)
  CHECK(got[5].calls == 1);
  CHECK(got[5].image == nullptr);

  // Everything else still completes, in order.
  int delivered = 0;
  while (rig.transport.pending() > 0) {
    rig.transport.deliver_at(0, 200, "OK");
    ++delivered;
  }
  CHECK(delivered == 2 + 64);
  CHECK(again.calls == 1);
  CHECK(again.image != nullptr);
  CHECK(rig.transport.requests[2].url == std::string(kBase) + path_n(6));
}

TEST_CASE("http images: a failed download answers nullptr and is remembered per cause") {
  struct Case {
    const char* name;
    int status;  // 0: transport error
    bool decodeFail;
    int64_t windowMs;
  };
  const Case cases[] = {
      {"404", 404, false, 5 * 60 * 1000},
      {"400", 400, false, 5 * 60 * 1000},
      {"decode failure", 200, true, 5 * 60 * 1000},
      {"502", 502, false, 30 * 1000},
      {"500", 500, false, 30 * 1000},
      {"transport error", 0, false, 30 * 1000},
      {"401", 401, false, 60 * 1000},
  };
  for (const Case& c : cases) {
    INFO("case: ", c.name);
    Rig rig;
    Got first;
    rig.provider.request(kCrest57, first.cb());
    if (c.status == 0) {
      rig.transport.deliver_error_at(0);
    } else {
      rig.transport.deliver_at(0, c.status, c.decodeFail ? "garbage" : "OK");
    }
    CHECK(first.calls == 1);
    CHECK(first.image == nullptr);
    CHECK(rig.transport.requests.size() == 1);

    // Inside the window: answered nullptr synchronously, nothing fetched.
    rig.provider.tick(c.windowMs - 1);
    Got within;
    rig.provider.request(kCrest57, within.cb());
    CHECK(within.calls == 1);
    CHECK(within.image == nullptr);
    CHECK(rig.transport.requests.size() == 1);

    // After the window: tried again.
    rig.provider.tick(c.windowMs);
    Got after;
    rig.provider.request(kCrest57, after.cb());
    CHECK(after.calls == 0);
    CHECK(rig.transport.requests.size() == 2);
    rig.transport.deliver_at(0, 200, "OK");
    CHECK(after.calls == 1);
    CHECK(after.image != nullptr);
  }
}

TEST_CASE("http images: a negative entry is per path") {
  Rig rig;
  rig.provider.request(kCrest57, [](ImagePtr) {});
  rig.transport.deliver_at(0, 404, "");
  Got other;
  rig.provider.request(kCrest61, other.cb());
  CHECK(rig.transport.requests.size() == 2);  // a different path is not affected
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(other.image != nullptr);
}

TEST_CASE("http images: a 401 sets auth_rejected, a later success clears it") {
  Rig rig;
  CHECK_FALSE(rig.provider.auth_rejected());
  rig.provider.request(kCrest57, [](ImagePtr) {});
  rig.transport.deliver_at(0, 404, "");
  CHECK_FALSE(rig.provider.auth_rejected());  // other failures do not
  rig.provider.request(kCrest61, [](ImagePtr) {});
  rig.transport.deliver_at(0, 401, "");
  CHECK(rig.provider.auth_rejected());

  rig.provider.tick(61000);
  rig.provider.request(kCrest61, [](ImagePtr) {});
  rig.transport.deliver_at(0, 200, "OK");
  CHECK_FALSE(rig.provider.auth_rejected());
}

TEST_CASE("http images: a request unanswered for 30 s is lost; its late answer is ignored") {
  Rig rig;
  Got a, b;
  rig.provider.request(kCrest57, a.cb());
  rig.provider.request(kCrest57, b.cb());
  rig.provider.tick(29999);
  CHECK(a.calls == 0);
  rig.provider.tick(30000);
  CHECK(a.calls == 1);
  CHECK(b.calls == 1);
  CHECK(a.image == nullptr);
  CHECK(b.image == nullptr);
  CHECK(rig.provider.pending() == 0);

  // The late answer arrives: nothing happens.
  CHECK(rig.transport.deliver_at(0, 200, "OK"));
  CHECK(a.calls == 1);
  CHECK(rig.provider.cached_bytes() == 0);
  CHECK(rig.decoder.calls == 0);

  // A lost request is remembered like a transport error: not retried at once.
  Got c;
  rig.provider.request(kCrest57, c.cb());
  CHECK(c.calls == 1);
  CHECK(c.image == nullptr);
  CHECK(rig.transport.requests.size() == 1);
  rig.provider.tick(60000);
  rig.provider.request(kCrest57, c.cb());
  CHECK(rig.transport.requests.size() == 2);
}

TEST_CASE("http images: a lost request frees its slot, and a late answer cannot hit the retry") {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.provider.request(path_n(i), [](ImagePtr) {});
  REQUIRE(rig.transport.requests.size() == 2);  // path 2 waits
  rig.provider.tick(30000);                      // 0 and 1 are lost, 2 starts
  REQUIRE(rig.transport.requests.size() == 3);
  CHECK(rig.transport.requests[2].url == std::string(kBase) + path_n(2));

  // After the window path 0 is requested again (new request id). The old
  // answer for path 0 (pending index 0) must not complete it.
  rig.provider.tick(60000);  // path 2 (started at 30000) is lost too
  Got retry;
  rig.provider.request(path_n(0), retry.cb());
  REQUIRE(rig.transport.requests.size() == 4);
  rig.transport.deliver_at(0, 200, "OK");  // the stale answer for the first path 0 attempt
  CHECK(retry.calls == 0);
  CHECK(rig.provider.pending() == 1);
  rig.transport.deliver_at(rig.transport.pending() - 1, 200, "OK");  // the real one
  CHECK(retry.calls == 1);
  CHECK(retry.image != nullptr);
}

TEST_CASE("http images: the clock is the tick, a request made before the first tick is not born old") {
  FakeTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider(transport, decoder, kBase, kToken);
  Got got;
  provider.request(kCrest57, got.cb());  // no tick yet
  provider.tick(500000);                 // the host's clock started long ago
  CHECK(got.calls == 0);
  provider.tick(500000 + 29999);
  CHECK(got.calls == 0);
  provider.tick(500000 + 30000);
  CHECK(got.calls == 1);
}

TEST_CASE("http images: eviction under a tiny budget does not invalidate a held image") {
  Rig rig(100);  // one 64-byte image fits, two do not
  Got a;
  rig.provider.request(kCrest57, a.cb());
  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(a.image);
  const lv_image_dsc_t* aRaw = a.image.get();
  CHECK(rig.provider.cached_bytes() == 64);

  Got b;
  rig.provider.request(kCrest61, b.cb());
  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(b.image);
  CHECK(rig.provider.cached_bytes() == 64);  // 57 was evicted

  // The held pointer still reads fine.
  CHECK(a.image.get() == aRaw);
  CHECK(a.image.use_count() == 1);  // the cache let go of it
  CHECK(a.image->header.w == 4);
  CHECK(a.image->data[0] == 0x40);
  CHECK(a.image->data[63] == 0x40 + 63);

  // 57 is a miss again.
  Got again;
  rig.provider.request(kCrest57, again.cb());
  CHECK(again.calls == 0);
  CHECK(rig.transport.requests.size() == 3);
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(again.image.get() != aRaw);
}

TEST_CASE("http images: an image bigger than the whole budget is delivered but not cached") {
  Rig rig(10);
  Got a;
  rig.provider.request(kCrest57, a.cb());
  rig.transport.deliver_at(0, 200, "OK");
  REQUIRE(a.image);
  CHECK(rig.provider.cached_bytes() == 0);
}

TEST_CASE("http images: destroying the provider with requests in flight is safe") {
  FakeTransport transport;
  FakeDecoder decoder;
  int calls = 0;
  {
    HttpImageProvider provider(transport, decoder, kBase, kToken);
    provider.tick(0);
    for (int i = 0; i < 5; ++i) provider.request(path_n(i), [&](ImagePtr) { ++calls; });
    REQUIRE(transport.pending() == 2);
  }
  // The transport still holds the callbacks; answering them is a no-op.
  CHECK(transport.deliver_at(0, 200, "OK"));
  CHECK(transport.deliver_at(0, 404, ""));
  CHECK(transport.deliver_error_at(0) == false);
  CHECK(calls == 0);
  CHECK(decoder.calls == 0);
  CHECK(transport.requests.size() == 2);  // nothing new was started
}

TEST_CASE("http images: a callback that destroys the provider does not break the other waiters") {
  FakeTransport transport;
  FakeDecoder decoder;
  auto provider = std::make_unique<HttpImageProvider>(transport, decoder, kBase, kToken);
  Got b;
  provider->request(kCrest57, [&](ImagePtr) { provider.reset(); });
  provider->request(kCrest57, b.cb());
  transport.deliver_at(0, 200, "OK");
  CHECK(provider == nullptr);
  CHECK(b.calls == 1);
  CHECK(b.image != nullptr);
}

TEST_CASE("http images: a done callback may request more images") {
  Rig rig;
  Got inner;
  Got innerCached;
  int outerCalls = 0;
  rig.provider.request(kCrest57, [&](ImagePtr img) {
    ++outerCalls;
    // A different path: queued and started while we are inside the callback.
    rig.provider.request(kCrest61, inner.cb());
    // The path that just completed: a cache hit, answered at once.
    rig.provider.request(kCrest57, innerCached.cb());
    CHECK(img != nullptr);
  });
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(outerCalls == 1);
  CHECK(innerCached.calls == 1);
  CHECK(innerCached.image != nullptr);
  REQUIRE(rig.transport.requests.size() == 2);
  CHECK(inner.calls == 0);
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(inner.calls == 1);
  CHECK(inner.image != nullptr);
}

TEST_CASE("http images: a done callback re-requesting a failed path gets nullptr, not a loop") {
  Rig rig;
  int calls = 0;
  std::function<void(ImagePtr)> cb = [&](ImagePtr img) {
    ++calls;
    CHECK(img == nullptr);
    if (calls < 5) rig.provider.request(kCrest57, cb);
  };
  rig.provider.request(kCrest57, cb);
  rig.transport.deliver_at(0, 404, "");
  CHECK(calls == 5);  // each retry answered at once from the negative cache
  CHECK(rig.transport.requests.size() == 1);
}

TEST_CASE("http images: a transport that answers from inside get() works") {
  SyncTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider(transport, decoder, kBase, kToken);
  provider.tick(0);
  std::vector<Got> got(6);
  for (int i = 0; i < 6; ++i) provider.request(path_n(i), got[i].cb());
  for (const Got& g : got) {
    CHECK(g.calls == 1);
    CHECK(g.image != nullptr);
  }
  CHECK(transport.urls.size() == 6);
  CHECK(provider.pending() == 0);
}

TEST_CASE("http images: the negative cache stays bounded") {
  Rig rig;
  for (int i = 0; i < 1000; ++i) {
    rig.provider.request(path_n(i), [](ImagePtr) {});
    rig.transport.deliver_at(0, 404, "");
  }
  CHECK(rig.provider.negative_entries() <= HttpImageProvider::kMaxNegative);
}

TEST_CASE("http images: the device bound of 64 remembered failures holds, and the newest failures stay") {
  FakeTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider(transport, decoder, kBase, kToken, HttpImageProvider::kDefaultCacheBytes,
                             HttpImageProvider::kDeviceMaxNegative);
  CHECK(HttpImageProvider::kDeviceMaxNegative == 64);
  int64_t now = 0;
  provider.tick(now);
  for (int i = 0; i < 300; ++i) {
    provider.request(path_n(i), [](ImagePtr) {});
    transport.deliver_at(0, 404, "");
    CHECK(provider.negative_entries() <= 64);
    provider.tick(++now);  // each failure expires a millisecond after the one before
  }
  CHECK(provider.negative_entries() == 64);
  // The last failure is still remembered: no new request for it.
  const size_t before = transport.requests.size();
  Got got;
  provider.request(path_n(299), got.cb());
  CHECK(got.calls == 1);
  CHECK(got.image == nullptr);
  CHECK(transport.requests.size() == before);
  // One evicted long ago is asked again.
  provider.request(path_n(0), [](ImagePtr) {});
  CHECK(transport.requests.size() == before + 1);
}

TEST_CASE("http images: a bound of 0 remembered failures is taken as 1") {
  FakeTransport transport;
  FakeDecoder decoder;
  HttpImageProvider provider(transport, decoder, kBase, kToken, HttpImageProvider::kDefaultCacheBytes, 0);
  provider.tick(0);
  for (int i = 0; i < 5; ++i) {
    provider.request(path_n(i), [](ImagePtr) {});
    transport.deliver_at(0, 500, "");
  }
  CHECK(provider.negative_entries() == 1);
}

TEST_CASE("http images: images load into widgets once delivered, and a deleted widget is safe") {
  init_test_display();
  {
    FakeTransport transport;
    FakeDecoder decoder;
    HttpImageProvider provider(transport, decoder, kBase, kToken);
    provider.tick(0);

    lv_obj_t* shown = lv_image_create(lv_screen_active());
    lv_obj_t* doomed = lv_image_create(lv_screen_active());
    image_load_into(shown, provider, kCrest57);
    image_load_into(doomed, provider, kCrest57);  // shares the one download
    REQUIRE(transport.requests.size() == 1);
    CHECK(lv_image_get_src(shown) == nullptr);  // placeholder while downloading

    lv_obj_delete(doomed);  // before the download completes
    test_advance(10);

    transport.deliver_at(0, 200, "OK");
    const auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(shown));
    REQUIRE(dsc != nullptr);
    CHECK(dsc->header.w == 4);
    test_advance(50);  // renders without crashing

    // A second widget for the same path is served from the cache.
    lv_obj_t* again = lv_image_create(lv_screen_active());
    image_load_into(again, provider, kCrest57);
    CHECK(lv_image_get_src(again) == lv_image_get_src(shown));
    CHECK(transport.requests.size() == 1);

    // A failing path leaves the placeholder.
    lv_obj_t* missing = lv_image_create(lv_screen_active());
    image_load_into(missing, provider, kCrest61);
    transport.deliver_at(0, 404, "");
    CHECK(lv_image_get_src(missing) == nullptr);

    lv_obj_delete(shown);
    lv_obj_delete(again);
    lv_obj_delete(missing);
    test_advance(10);
  }
  deinit_test_display();
}

TEST_CASE("http images: the path guard is a strict allow-list") {
  const std::string img = "/api/img?u=";
  const std::vector<std::string> rejected = {
      img + "a\xC3\xA9" "b",          // bytes >= 0x80 (raw UTF-8)
      img + "a\xFF" "b",
      img + "a\\b",                   // backslash
      img + "a#frag",                 // fragment
      img + "a\"b",                   // quotes
      img + "a'b",
      img + "a<b",
      img + "a>b",
      img + "a b",                    // space
      img + "a\tb",                   // control characters
      img + "a\x1F" "b",
      img + "a\x7F" "b",
      img + "a`b",
      img + "a{b}",
      img + "a|b",
      img + "a^b",
      img + "a[b]",
      img + "a;b",
      img + "a@b",
      img + "a!b",
      img + "a$b",
      img + "a*b",
      img + "a(b)",
      img + "%2e%2e/..",              // ".." still refused
      img + "https://x",              // "://" still refused
  };
  for (const std::string& p : rejected) {
    INFO("path: ", p);
    CHECK_FALSE(HttpImageProvider::is_allowed_path(p));
  }
  const std::vector<std::string> accepted = {
      "/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F359.png&w=48&h=48",
      "/api/cutout?name=Kai%20Havertz&w=96&h=96",
      "/api/cutout?name=J%C3%BCrgen%20Kl%C3%B6pp&w=96&h=96",  // percent-encoded UTF-8
      "/api/img?u=a-b_c.d~e%2F&fit=cover&w=240&h=135",
      "/api/img?u=a+b,c:d/e",
  };
  for (const std::string& p : accepted) {
    INFO("path: ", p);
    CHECK(HttpImageProvider::is_allowed_path(p));
  }
}

TEST_CASE("http images: a clock that jumps backwards does not freeze lost-request expiry") {
  Rig rig;
  Got a;
  rig.provider.tick(500000);
  rig.provider.request(kCrest57, a.cb());  // started at 500000, never answered
  rig.provider.tick(1000);                 // the clock went back
  rig.provider.tick(30999);
  CHECK(a.calls == 0);
  rig.provider.tick(31000);  // lost 30 s after the new base
  CHECK(a.calls == 1);
  CHECK(a.image == nullptr);
  CHECK(rig.provider.pending() == 0);
  // The remembered failure expires on the new clock too.
  Got b;
  rig.provider.tick(31000 + HttpImageProvider::kNegativeShortMs);
  rig.provider.request(kCrest57, b.cb());
  CHECK(rig.transport.requests.size() == 2);
}

TEST_CASE("http images: a backwards jump does not keep old failures for the length of the jump") {
  Rig rig;
  rig.provider.tick(1000000);
  Got a;
  rig.provider.request(kCrest57, a.cb());
  REQUIRE(rig.transport.deliver_at(0, 404, ""));
  REQUIRE(a.calls == 1);
  rig.provider.tick(0);  // back by 1000 s
  rig.provider.tick(HttpImageProvider::kNegativeLongMs);
  Got b;
  rig.provider.request(kCrest57, b.cb());
  CHECK(rig.transport.requests.size() == 2);  // tried again after at most the 5 min window
}

TEST_CASE("http images: offline, requests wait in the queue and go out once online") {
  Rig rig;
  CHECK(rig.provider.online());
  rig.provider.set_online(false);
  Got a, b, c;
  rig.provider.request(kCrest57, a.cb());
  rig.provider.request(kCrest61, b.cb());
  rig.provider.request(kCrest57, c.cb());  // joins the waiting one
  CHECK(rig.transport.requests.empty());
  CHECK(rig.provider.pending() == 2);
  CHECK(rig.provider.in_flight() == 0);
  // Waiting offline is not "lost": nothing expires however long it takes.
  rig.provider.tick(10 * HttpImageProvider::kLostMs);
  CHECK(a.calls == 0);
  CHECK(rig.provider.pending() == 2);

  rig.provider.set_online(true);
  REQUIRE(rig.transport.requests.size() == 2);
  CHECK(rig.transport.requests[0].url == std::string(kBase) + kCrest57);
  CHECK(rig.provider.in_flight() == 2);
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(a.calls == 1);
  CHECK(c.calls == 1);
  CHECK(a.image != nullptr);
  CHECK(a.image == c.image);
  CHECK(rig.provider.cached_images() == 1);
  CHECK(rig.provider.cached_bytes() == 64);
}

TEST_CASE("http images: going offline lets requests in flight finish but sends no new one") {
  Rig rig;
  Got got[4];
  for (int i = 0; i < 4; ++i) rig.provider.request(path_n(i), got[i].cb());
  CHECK(rig.transport.requests.size() == 2);  // kMaxConcurrent
  rig.provider.set_online(false);
  rig.transport.deliver_at(0, 200, "OK");
  rig.transport.deliver_at(0, 200, "OK");
  CHECK(got[0].image != nullptr);
  CHECK(got[1].image != nullptr);
  CHECK(rig.transport.requests.size() == 2);  // the other two still wait
  CHECK(rig.provider.pending() == 2);
  rig.provider.set_online(true);
  CHECK(rig.transport.requests.size() == 4);
}

TEST_CASE("http images: offline, the queue bound still answers the oldest waiting request") {
  Rig rig;
  rig.provider.set_online(false);
  std::vector<Got> got(HttpImageProvider::kMaxQueued + 1);
  for (size_t i = 0; i < got.size(); ++i) rig.provider.request(path_n(static_cast<int>(i)), got[i].cb());
  CHECK(got[0].calls == 1);
  CHECK(got[0].image == nullptr);
  CHECK(got[1].calls == 0);
  CHECK(rig.provider.pending() == HttpImageProvider::kMaxQueued);
  CHECK(rig.transport.requests.empty());
}
