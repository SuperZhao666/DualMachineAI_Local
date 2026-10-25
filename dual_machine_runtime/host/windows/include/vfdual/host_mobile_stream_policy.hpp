#pragma once

#include "vfdual/protocol.hpp"

#include <cstdint>
#include <string>

namespace vfdual {

/** Production VF Mobile compatibility only; generic video agents accept other ROIs. */
[[nodiscard]] inline bool validate_host_mobile_capture_geometry(
    std::uint32_t capture_width, std::uint32_t capture_height,
    std::uint32_t source_width, std::uint32_t source_height,
    std::string& error) {
    if (capture_width == kProductionCaptureWidth &&
        capture_height == kProductionCaptureHeight) {
        return true;
    }
    error = "VF Mobile requires an unpadded " +
        std::to_string(kProductionCaptureWidth) + "x" +
        std::to_string(kProductionCaptureHeight) +
        " physical capture region; actual_physical_roi=" +
        std::to_string(capture_width) + "x" + std::to_string(capture_height) +
        " display_size=" + std::to_string(source_width) + "x" +
        std::to_string(source_height) +
        "; the selected display must provide at least " +
        std::to_string(kProductionCaptureWidth) + "x" +
        std::to_string(kProductionCaptureHeight) +
        " physical pixels; failure_stage=mobile_capture_geometry";
    return false;
}

}  // namespace vfdual
