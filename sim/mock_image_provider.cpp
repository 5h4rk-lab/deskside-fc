#include "mock_image_provider.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "src/draw/lv_image_decoder_private.h"
#include "src/libs/tjpgd/tjpgd.h"

namespace {

// --- Path to asset name ------------------------------------------------------

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes %XX sequences; malformed sequences are kept literally.
std::string percent_decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// Value of query parameter `key` (percent-decoded), or false when absent.
bool query_param(const std::string& query, const std::string& key, std::string& value) {
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t end = query.find('&', pos);
    if (end == std::string::npos) end = query.size();
    const std::string pair = query.substr(pos, end - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.compare(0, eq, key) == 0) {
      value = percent_decode(pair.substr(eq + 1));
      return true;
    }
    pos = end + 1;
  }
  return false;
}

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

// "<id>.png" -> "<id>" when the name is digits plus ".png".
bool png_id(const std::string& file, std::string& id) {
  if (!ends_with(file, ".png")) return false;
  id = file.substr(0, file.size() - 4);
  return all_digits(id);
}

std::string image_asset_name(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  const size_t hostStart = scheme + 3;
  const size_t slash = url.find('/', hostStart);
  if (slash == std::string::npos) return "";
  const std::string host = url.substr(hostStart, slash - hostStart);
  size_t pathEnd = url.find_first_of("?#", slash);
  if (pathEnd == std::string::npos) pathEnd = url.size();
  const std::string urlPath = url.substr(slash, pathEnd - slash);
  const std::string base = urlPath.substr(urlPath.rfind('/') + 1);

  std::string id;
  if (host == "crests.football-data.org" && urlPath.find('/', 1) == std::string::npos &&
      png_id(base, id)) {
    return "crest-" + id + ".png";
  }
  if (host == "a.espncdn.com" && starts_with(urlPath, "/i/teamlogos/soccer/500/") &&
      urlPath.find('/', 24) == std::string::npos && png_id(base, id)) {
    return "espn-" + id + ".png";
  }
  if (base.empty() || base == "." || base == "..") return "";
  std::string safe;
  for (char c : base) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    safe.push_back(ok ? c : '_');
  }
  return "img-" + safe;
}

std::string cutout_asset_name(const std::string& name) {
  if (name.empty()) return "";
  std::string out = "cutout-";
  for (unsigned char c : name) {
    if ((c & 0xC0) == 0x80) continue;  // continuation byte: part of the previous character
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    out.push_back(alnum ? static_cast<char>(c) : '_');
  }
  return out + ".png";
}

// --- Decoding ----------------------------------------------------------------

constexpr size_t kMaxFileBytes = 8u * 1024 * 1024;
constexpr uint32_t kMaxDimension = 2048;

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty() && out.size() <= kMaxFileBytes;
}

// Image descriptor plus its pixel memory, freed together when the last
// ImagePtr goes away.
struct OwnedImage {
  lv_image_dsc_t dsc{};
  std::vector<uint8_t> pixels;
};

ImagePtr own(int32_t w, int32_t h, uint32_t stride, std::vector<uint8_t> pixels) {
  auto owned = std::make_shared<OwnedImage>();
  owned->pixels = std::move(pixels);
  owned->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  owned->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  owned->dsc.header.w = w;
  owned->dsc.header.h = h;
  owned->dsc.header.stride = stride;
  owned->dsc.data_size = static_cast<uint32_t>(owned->pixels.size());
  owned->dsc.data = owned->pixels.data();
  return ImagePtr(owned, &owned->dsc);  // aliasing: the control block owns OwnedImage
}

ImagePtr decode_png(const std::vector<uint8_t>& file) {
  lv_image_dsc_t src{};
  src.header.magic = LV_IMAGE_HEADER_MAGIC;
  src.header.cf = LV_COLOR_FORMAT_RAW;
  src.data = file.data();
  src.data_size = static_cast<uint32_t>(file.size());

  lv_image_decoder_args_t args{};
  args.no_cache = true;  // we do our own caching
  lv_image_decoder_dsc_t dsc;
  if (lv_image_decoder_open(&dsc, &src, &args) != LV_RESULT_OK) return nullptr;

  ImagePtr result;
  const lv_draw_buf_t* decoded = dsc.decoded;
  if (decoded && decoded->data && decoded->header.cf == LV_COLOR_FORMAT_ARGB8888 &&
      decoded->header.w > 0 && decoded->header.h > 0 &&
      static_cast<uint32_t>(decoded->header.w) <= kMaxDimension &&
      static_cast<uint32_t>(decoded->header.h) <= kMaxDimension) {
    const uint32_t stride = decoded->header.stride ? decoded->header.stride : decoded->header.w * 4;
    const size_t bytes = static_cast<size_t>(stride) * decoded->header.h;
    std::vector<uint8_t> pixels(decoded->data, decoded->data + bytes);
    result = own(decoded->header.w, decoded->header.h, stride, std::move(pixels));
  }
  lv_image_decoder_close(&dsc);
  return result;
}

struct JpegContext {
  const uint8_t* data;
  size_t size;
  size_t pos;
  std::vector<uint8_t> pixels;  // ARGB8888 (B, G, R, A in memory)
  uint32_t width;
};

size_t jpeg_in(JDEC* jd, uint8_t* buf, size_t n) {
  auto* in = static_cast<JpegContext*>(jd->device);
  const size_t avail = in->size - in->pos;
  if (n > avail) n = avail;
  if (buf) std::memcpy(buf, in->data + in->pos, n);
  in->pos += n;
  return n;
}

int jpeg_out(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* out = static_cast<JpegContext*>(jd->device);
  // LVGL's tjpgd emits 3 bytes per pixel in LV_COLOR_FORMAT_RGB888 order (B, G, R).
  const uint8_t* bgr = static_cast<const uint8_t*>(bitmap);
  for (uint32_t y = rect->top; y <= rect->bottom; ++y) {
    for (uint32_t x = rect->left; x <= rect->right; ++x) {
      uint8_t* p = &out->pixels[(static_cast<size_t>(y) * out->width + x) * 4];
      p[0] = bgr[0];
      p[1] = bgr[1];
      p[2] = bgr[2];
      p[3] = 0xFF;
      bgr += 3;
    }
  }
  return 1;
}

ImagePtr decode_jpeg(const std::vector<uint8_t>& file) {
  std::vector<uint8_t> work(4096);
  JpegContext ctx{file.data(), file.size(), 0, {}, 0};
  JDEC jd;
  if (jd_prepare(&jd, jpeg_in, work.data(), work.size(), &ctx) != JDR_OK) return nullptr;
  if (jd.width == 0 || jd.height == 0 || jd.width > kMaxDimension || jd.height > kMaxDimension) {
    return nullptr;
  }
  ctx.width = jd.width;
  ctx.pixels.assign(static_cast<size_t>(jd.width) * jd.height * 4, 0);
  if (jd_decomp(&jd, jpeg_out, 0) != JDR_OK) return nullptr;
  return own(jd.width, jd.height, jd.width * 4, std::move(ctx.pixels));
}

}  // namespace

std::string mock_asset_name(const std::string& path) {
  const size_t q = path.find('?');
  if (q == std::string::npos) return "";
  const std::string route = path.substr(0, q);
  const std::string query = path.substr(q + 1);
  std::string value;
  if (route == "/api/img") {
    if (!query_param(query, "u", value) || value.empty()) return "";
    return image_asset_name(value);
  }
  if (route == "/api/cutout") {
    if (!query_param(query, "name", value)) return "";
    return cutout_asset_name(value);
  }
  return "";
}

MockImageProvider::MockImageProvider(std::string assetDir, size_t cacheBudget)
    : dir_(std::move(assetDir)), cache_(cacheBudget) {}

ImagePtr MockImageProvider::load(const std::string& name) const {
  std::vector<uint8_t> file;
  if (!read_file(dir_ + "/" + name, file)) return nullptr;
  static const uint8_t kPngMagic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (file.size() > sizeof(kPngMagic) && std::memcmp(file.data(), kPngMagic, sizeof(kPngMagic)) == 0) {
    return decode_png(file);
  }
  if (file.size() > 3 && file[0] == 0xFF && file[1] == 0xD8) return decode_jpeg(file);
  return nullptr;
}

void MockImageProvider::request(const std::string& path, std::function<void(ImagePtr)> done) {
  const std::string name = mock_asset_name(path);
  if (name.empty()) return done(nullptr);

  if (ImagePtr* hit = cache_.get(name)) {
    ImagePtr image = *hit;  // copy before the callback can touch the cache
    return done(std::move(image));
  }
  ImagePtr image = load(name);
  if (!image) return done(nullptr);
  cache_.put(name, image, image->data_size);
  done(std::move(image));
}
