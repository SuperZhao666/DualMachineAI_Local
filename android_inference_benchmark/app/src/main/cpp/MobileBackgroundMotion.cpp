#include "MobileBackgroundMotion.hpp"
#include "MobilePatchSad.hpp"
#include "MobileLargeBackgroundMotion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vfdual_android {
namespace {
constexpr int kRadius = 3;
static_assert(kRadius == 3, "patch_sad7 requires a seven-pixel patch");
constexpr int kSearch = 16;
constexpr int kGrid = 7;
constexpr int kPatchPixels = (2 * kRadius + 1) * (2 * kRadius + 1);
struct Match { float x{}, y{}; int point_x{}, point_y{}; };

int patch_sad(const std::vector<std::uint8_t>& a,
              const std::vector<std::uint8_t>& b, int width,
              int x, int y, int dx, int dy) noexcept {
  const auto offset = (y-kRadius)*width+x-kRadius;
  return patch_sad7(a.data()+offset,b.data()+offset+dy*width+dx,std::size_t(width));
}

float parabola_offset(int negative, int center, int positive) noexcept {
  const float denominator = float(negative + positive - 2 * center);
  if (denominator <= 0.0F) return 0.0F;
  return std::clamp(0.5F * float(negative - positive) / denominator, -0.5F, 0.5F);
}
}  // namespace

BackgroundMotion MobileBackgroundMotion::observe(
    std::span<const std::uint8_t> rgb, std::uint32_t width,
    std::uint32_t height, std::span<const YoloDetection> detections,
    std::uint64_t observed_at_us, std::uint64_t generation,
    std::uint64_t recovery_budget_us) {
  BackgroundMotion result{};
  if (width < 64U || height < 64U || width > 1024U || height > 1024U ||
      rgb.size() != std::size_t(width) * height * 6U ||
      observed_at_us == 0U || generation == 0U) {
    reset();
    return result;
  }
  const auto scale = std::max(2U, (std::max(width, height) + 207U) / 208U);
  const auto small_width = width / scale;
  const auto small_height = height / scale;
  if (small_width < 64U || small_height < 64U) { reset(); return result; }
  const auto count = std::size_t(small_width) * small_height;
  const bool continuous = generation_ == generation && scale_ == scale &&
      width_ == small_width && height_ == small_height && observed_at_us_ != 0U &&
      observed_at_us > observed_at_us_ && observed_at_us - observed_at_us_ <= 60'000U;
  // The eighth SIMD lane is discarded, but must still be readable at the
  // final row/column. Preserve the logical image dimensions and add one byte.
  current_.resize(count+1U);
  current_[count]=0U;
  current_mask_.assign(count, 0U);
  for (std::uint32_t y = 0; y < small_height; ++y) {
    for (std::uint32_t x = 0; x < small_width; ++x) {
      // RGB is byte*257 little-endian, so its low byte is exact original RGB.
      const auto pixel = (std::size_t(y * scale) * width + x * scale) * 6U;
      current_[std::size_t(y) * small_width + x] = static_cast<std::uint8_t>(
          (77U * rgb[pixel] + 150U * rgb[pixel + 2U] + 29U * rgb[pixel + 4U]) >> 8U);
    }
  }
  const auto mask_box = [&](float x1, float y1, float x2, float y2) {
    const int left = std::clamp(int(std::floor(x1 / scale)) - kRadius - 2, 0, int(small_width));
    const int top = std::clamp(int(std::floor(y1 / scale)) - kRadius - 2, 0, int(small_height));
    const int right = std::clamp(int(std::ceil(x2 / scale)) + kRadius + 2, 0, int(small_width));
    const int bottom = std::clamp(int(std::ceil(y2 / scale)) + kRadius + 2, 0, int(small_height));
    for (int y = top; y < bottom; ++y)
      std::fill(current_mask_.begin() + y * small_width + left,
                current_mask_.begin() + y * small_width + right, 1U);
  };
  mask_box(width * 0.5F - 9.0F, height * 0.5F - 9.0F,
           width * 0.5F + 9.0F, height * 0.5F + 9.0F);
  for (const auto& box : detections) {
    if (std::isfinite(box.x1) && std::isfinite(box.y1) &&
        std::isfinite(box.x2) && std::isfinite(box.y2) &&
        box.x2 >= box.x1 && box.y2 >= box.y1) {
      mask_box(std::clamp(box.x1, 0.0F, float(width)), std::clamp(box.y1, 0.0F, float(height)),
               std::clamp(box.x2, 0.0F, float(width)), std::clamp(box.y2, 0.0F, float(height)));
    }
  }
  if (continuous) {
    result.previous_observed_at_us = observed_at_us_;
    std::array<Match, kGrid * kGrid> matches{};
    std::size_t matched{};
    const int margin = kSearch + kRadius + 2;
    for (int gy = 0; gy < kGrid; ++gy) {
      for (int gx = 0; gx < kGrid; ++gx) {
        // Select the strongest two-dimensional texture in a small cell.
        const int center_x = margin + (int(small_width) - 2 * margin) * (2 * gx + 1) / (2 * kGrid);
        const int center_y = margin + (int(small_height) - 2 * margin) * (2 * gy + 1) / (2 * kGrid);
        int x = center_x, y = center_y, best_texture{};
        for (int oy = -3; oy <= 3; oy += 3) for (int ox = -3; ox <= 3; ox += 3) {
          const int px = center_x + ox, py = center_y + oy;
          if (previous_mask_[py * small_width + px]) continue;
          int xx{}, yy{}, xy{};
          for (int row = -2; row <= 2; ++row) for (int col = -2; col <= 2; ++col) {
            const auto i = (py + row) * small_width + px + col;
            const int ix = int(previous_[i + 1]) - int(previous_[i - 1]);
            const int iy = int(previous_[i + small_width]) - int(previous_[i - small_width]);
            xx += ix * ix; yy += iy * iy; xy += ix * iy;
          }
          const int texture = int((double(xx) * yy - double(xy) * xy) / (xx + yy + 1));
          if (texture > best_texture) { best_texture = texture; x = px; y = py; }
        }
        if (best_texture < 800) continue;
        ++result.candidates;
        int best = std::numeric_limits<int>::max(), bx{}, by{};
        int low_x = -kSearch, high_x = kSearch, low_y = -kSearch, high_y = kSearch;
        if (matched >= 10U) {
          std::array<float, kGrid * kGrid> xs{}, ys{};
          for (std::size_t i = 0; i < matched; ++i) { xs[i] = matches[i].x; ys[i] = matches[i].y; }
          std::sort(xs.begin(), xs.begin() + matched); std::sort(ys.begin(), ys.begin() + matched);
          const auto mx = xs[matched / 2U], my = ys[matched / 2U];
          unsigned support{};
          for (std::size_t i = 0; i < matched; ++i)
            if (std::hypot(matches[i].x - mx, matches[i].y - my) <= 1.5F) ++support;
          if (support >= 8U && support * 4 >= matched * 3) {
            low_x = std::max(-kSearch, int(std::lround(mx)) - 3);
            high_x = std::min(kSearch, int(std::lround(mx)) + 3);
            low_y = std::max(-kSearch, int(std::lround(my)) - 3);
            high_y = std::min(kSearch, int(std::lround(my)) + 3);
          }
        }
        // Search every reduced-resolution offset: skipping odd offsets aliases
        // fine textures and can choose a completely different correlation peak.
        for (int dy = low_y; dy <= high_y; ++dy) {
          for (int dx = low_x; dx <= high_x; ++dx) {
            if (current_mask_[(y + dy) * small_width + x + dx]) continue;
            const int score = patch_sad(previous_, current_, small_width, x, y, dx, dy);
            if (score < best) { best = score; bx = dx; by = dy; }
          }
        }
        if (best > 30 * kPatchPixels || std::abs(bx) == kSearch || std::abs(by) == kSearch) continue;
        // Reject repetitive/ambiguous texture: a distant peak must be worse.
        int alternative = std::numeric_limits<int>::max();
        for (int dy = -kSearch; dy <= kSearch; dy += 4) for (int dx = -kSearch; dx <= kSearch; dx += 4) {
          if (std::abs(dx - bx) <= 2 && std::abs(dy - by) <= 2) continue;
          alternative = std::min(alternative, patch_sad(previous_, current_, small_width, x, y, dx, dy));
        }
        if (alternative < best + 2 * kPatchPixels) continue;
        matches[matched++] = {
            float(bx) + parabola_offset(patch_sad(previous_, current_, small_width, x, y, bx - 1, by), best,
                                       patch_sad(previous_, current_, small_width, x, y, bx + 1, by)),
            float(by) + parabola_offset(patch_sad(previous_, current_, small_width, x, y, bx, by - 1), best,
                                       patch_sad(previous_, current_, small_width, x, y, bx, by + 1)), x, y};
      }
    }
    if (matched >= 6U) {
      std::array<float, kGrid * kGrid> xs{}, ys{};
      for (std::size_t i = 0; i < matched; ++i) { xs[i] = matches[i].x; ys[i] = matches[i].y; }
      std::sort(xs.begin(), xs.begin() + matched);
      std::sort(ys.begin(), ys.begin() + matched);
      const float mx = xs[matched / 2U], my = ys[matched / 2U];
      float sum_x{}, sum_y{};
      int min_x = int(small_width), max_x{}, min_y = int(small_height), max_y{};
      for (std::size_t i = 0; i < matched; ++i) {
        if (std::hypot(matches[i].x - mx, matches[i].y - my) > 1.5F) continue;
        sum_x += matches[i].x; sum_y += matches[i].y; ++result.support;
        min_x = std::min(min_x, matches[i].point_x); max_x = std::max(max_x, matches[i].point_x);
        min_y = std::min(min_y, matches[i].point_y); max_y = std::max(max_y, matches[i].point_y);
      }
      if (result.support >= 6U && result.support * 2U >= matched &&
          max_x - min_x > int(small_width) / 4 && max_y - min_y > int(small_height) / 4) {
        result.x = sum_x * scale / result.support;
        result.y = sum_y * scale / result.support;
        result.valid = true;
      }
    }
  }
  if(continuous&&!result.valid) {
    const auto recovered=large_background::recover(
        {previous_,previous_mask_,int(small_width),int(small_height)},
        {current_,current_mask_,int(small_width),int(small_height)},
        scale,observed_at_us_,recovery_budget_us);
    if(recovered.valid)result=recovered;
  }
  previous_.swap(current_);
  previous_mask_.swap(current_mask_);
  width_ = small_width; height_ = small_height; scale_ = scale;
  observed_at_us_ = observed_at_us; generation_ = generation;
  return result;
}

void MobileBackgroundMotion::reset() noexcept {
  observed_at_us_ = generation_ = 0U;
}
}  // namespace vfdual_android
