// eye_model.h -- the open/closed eye classifier (Intel Open Model Zoo
// open-closed-eye-0001, Apache-2.0) as plain C++.
#pragma once
#include <cstdint>

namespace saycheese {

/** Probability the eye in a 32x32 grey crop is open. */
double eyeOpenProbability(const uint8_t gray[32 * 32]);

} // namespace saycheese
