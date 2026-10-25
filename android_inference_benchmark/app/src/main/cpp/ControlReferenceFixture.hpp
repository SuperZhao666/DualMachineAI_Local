#pragma once
#include "MobileControlCore.hpp"

// Historical synthetic scenes and recorded regression traces use 416-space.
// Keep that coordinate system explicit when production model dimensions change.
// New 640/1024 contract tests exercise production dimensions separately.
inline vfdual_android::MobileControlConfig reference_416_control_config() {
  vfdual_android::MobileControlConfig value;
  value.model_width=value.model_height=416.F;
  value.model_center_x=value.model_center_y=208.F;
  return value;
}
inline constexpr vfdual::MobileModelContract reference_416_model_contract() {
  auto model=vfdual::kOverwatch2;
  model.input.width=model.input.height=416U;
  model.output.anchor_count=3549U;
  return model;
}
