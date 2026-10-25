#pragma once
#include <algorithm>
#include <cmath>
namespace ayt::audio {
struct StereoPanGains {float left=1,right=1;};
/// Equal-power stereo direction, -1 left / 0 center / +1 right.
/// Pure, allocation-free and device independent. Non-finite input is centered.
inline StereoPanGains equalPowerStereoPan(float pan) noexcept {
    const float angle=(std::clamp(std::isfinite(pan)?pan:0.0f,-1.0f,1.0f)+1.0f)*0.7853981633974483f;
    return {std::max(0.0f,std::cos(angle)),std::max(0.0f,std::sin(angle))};
}
}
