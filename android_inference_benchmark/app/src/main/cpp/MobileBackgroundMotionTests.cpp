#include "MobileBackgroundMotion.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace vfdual_android;
namespace {
constexpr int kWidth = 416;
std::vector<std::uint8_t> scene(unsigned seed = 7U) {
  std::vector<std::uint8_t> noise(kWidth * kWidth), smooth(noise.size());
  for (auto& value : noise) { seed = 1664525U * seed + 1013904223U; value = seed >> 24U; }
  for (int y = 2; y < kWidth - 2; ++y) for (int x = 2; x < kWidth - 2; ++x) {
    int sum{};
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) sum += noise[(y + dy) * kWidth + x + dx];
    smooth[y * kWidth + x] = static_cast<std::uint8_t>(std::clamp(128 + (sum / 25 - 128) * 3, 0, 255));
  }
  return smooth;
}
std::vector<std::uint8_t> rgb(const std::vector<std::uint8_t>& image, float dx = 0, float dy = 0) {
  std::vector<std::uint8_t> result(kWidth * kWidth * 6U);
  for (int y = 0; y < kWidth; ++y) for (int x = 0; x < kWidth; ++x) {
    const float sx = std::clamp(x - dx, 0.0F, float(kWidth - 2));
    const float sy = std::clamp(y - dy, 0.0F, float(kWidth - 2));
    const int ix = int(sx), iy = int(sy);
    const float fx = sx - ix, fy = sy - iy;
    const auto value = static_cast<std::uint8_t>(
        image[iy * kWidth + ix] * (1 - fx) * (1 - fy) + image[iy * kWidth + ix + 1] * fx * (1 - fy) +
        image[(iy + 1) * kWidth + ix] * (1 - fx) * fy + image[(iy + 1) * kWidth + ix + 1] * fx * fy);
    std::fill_n(result.begin() + (y * kWidth + x) * 6U, 6U, value);
  }
  return result;
}
void test_translation_and_discontinuity() {
  const auto background = scene();
  for (const auto displacement : {std::pair{0.0F, 0.0F}, {7.0F, -3.0F}, {-24.0F, 18.0F}, {1.4F, 0.7F}}) {
    MobileBackgroundMotion estimator;
    assert(!estimator.observe(rgb(background), kWidth, kWidth, {}, 100'000, 1).valid);
    const auto result = estimator.observe(rgb(background, displacement.first, displacement.second), kWidth, kWidth, {}, 107'000, 1);
    std::printf("background requested=%.2f,%.2f measured=%.3f,%.3f support=%u valid=%u\n", displacement.first, displacement.second, result.x, result.y, result.support, result.valid);
    assert(result.valid && result.previous_observed_at_us == 100'000);
    assert(std::abs(result.x - displacement.first) < 0.8F);
    assert(std::abs(result.y - displacement.second) < 0.8F);
    assert(!estimator.observe(rgb(background), kWidth, kWidth, {}, 200'000, 1).valid);
    assert(!estimator.observe(rgb(background), kWidth, kWidth, {}, 207'000, 2).valid);
    assert(!estimator.observe(rgb(background), kWidth, kWidth, {}, 207'000, 2).valid);
  }
}
void test_masked_independent_foreground_and_untrusted_scene() {
  MobileBackgroundMotion estimator;
  const auto a = scene();
  auto first = rgb(a), second = rgb(a, -12, 6);
  const std::array boxes{YoloDetection{110, 75, 280, 330, .9F, 0}};
  for (int y = 75; y < 330; ++y) for (int x = 110; x < 280; ++x) {
    std::fill_n(first.begin() + (y * kWidth + x) * 6U, 6, static_cast<std::uint8_t>((x + y) % 255));
    std::fill_n(second.begin() + (y * kWidth + x) * 6U, 6, static_cast<std::uint8_t>((x * 3 + y) % 255));
  }
  static_cast<void>(estimator.observe(first, kWidth, kWidth, boxes, 100'000, 1));
  const auto result = estimator.observe(second, kWidth, kWidth, boxes, 107'000, 1);
  assert(result.valid && std::abs(result.x + 12) < .8F && std::abs(result.y - 6) < .8F);
  assert(!estimator.observe(rgb(scene(777U)), kWidth, kWidth, {}, 114'000, 1).valid);
  const std::vector<std::uint8_t> flat(kWidth * kWidth * 6U, 114);
  static_cast<void>(estimator.observe(flat, kWidth, kWidth, {}, 121'000, 1));
  assert(!estimator.observe(flat, kWidth, kWidth, {}, 128'000, 1).valid);
  assert(!estimator.observe({}, kWidth, kWidth, {}, 135'000, 1).valid);
}
void benchmark() {
  MobileBackgroundMotion estimator;
  const auto input = rgb(scene());
  std::vector<long long> times;
  for (unsigned i = 0; i < 100; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = estimator.observe(input, kWidth, kWidth, {}, 100'000 + i * 7'143U, 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    if (i > 0) { assert(result.valid); times.push_back(elapsed); }
  }
  std::sort(times.begin(), times.end());
  std::printf("background desktop benchmark p50_us=%lld p95_us=%lld\n", times[49], times[94]);
}
}
int main() { std::setvbuf(stdout, nullptr, _IONBF, 0); test_translation_and_discontinuity(); test_masked_independent_foreground_and_untrusted_scene(); benchmark(); }
