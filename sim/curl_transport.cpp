#include "curl_transport.h"

#include <curl/curl.h>

#include <mutex>
#include <utility>

namespace {

constexpr const char* kUserAgent = "soccer-display-sim/0.1";

// libcurl's global state, set up once for the process (not thread safe, so it
// happens before any worker starts).
void global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Body {
  std::string data;
  bool tooBig = false;
};

size_t on_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<Body*>(userdata);
  const size_t n = size * nmemb;
  if (body->data.size() + n > CurlTransport::kMaxBodyBytes) {
    body->tooBig = true;
    return 0;  // aborts the transfer (CURLE_WRITE_ERROR)
  }
  body->data.append(ptr, n);
  return n;
}

// How often a transfer in flight checks for shutdown.
constexpr int kStopCheckMs = 20;

}  // namespace

CurlTransport::CurlTransport(unsigned workers) {
  global_init();
  if (workers < 1) workers = 1;
  if (workers > kMaxWorkers) workers = kMaxWorkers;
  for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { run(); });
}

CurlTransport::~CurlTransport() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  for (std::thread& t : workers_) t.join();
  completed_.clear();
}

void CurlTransport::get(const std::string& url, const std::string& token, int timeoutMs,
                        std::function<void(HttpResult)> done) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.push_back({url, token, timeoutMs, std::move(done)});
  }
  cv_.notify_one();
}

void CurlTransport::poll() {
  std::deque<Done> ready;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ready.swap(completed_);
  }
  // No lock held: a callback may call get() again.
  for (Done& d : ready) d.done(std::move(d.result));
}

void CurlTransport::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    HttpResult result = perform(job);
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_) return;
    completed_.push_back({std::move(job.done), std::move(result)});
  }
}

HttpResult CurlTransport::perform(const Job& job) {
  HttpResult result;
  result.transportError = true;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) return result;

  Body body;
  // The token only ever goes into this header list (never into logs: verbose
  // output stays off).
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("x-token: " + job.token).c_str());
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(job.timeoutMs));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxBodyBytes));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  // Driven through a multi handle so shutdown can abort it within kStopCheckMs.
  CURLcode rc = CURLE_FAILED_INIT;
  CURLM* multi = curl_multi_init();
  if (multi != nullptr && curl_multi_add_handle(multi, curl) == CURLM_OK) {
    int running = 1;
    while (running > 0 && !stop_) {
      if (curl_multi_perform(multi, &running) != CURLM_OK) break;
      if (running > 0) curl_multi_poll(multi, nullptr, 0, kStopCheckMs, nullptr);
    }
    int left = 0;
    while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
      if (msg->msg == CURLMSG_DONE && msg->easy_handle == curl) rc = msg->data.result;
    }
    curl_multi_remove_handle(multi, curl);
  }
  if (multi != nullptr) curl_multi_cleanup(multi);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || body.tooBig || status == 0) return result;
  result.transportError = false;
  result.status = static_cast<int>(status);
  result.body = std::move(body.data);
  return result;
}
