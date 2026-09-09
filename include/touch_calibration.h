/**
 * @file touch_calibration.h
 * @brief Interactive touch calibration and orientation verification.
 *
 * Kept separate from the application because it is a maintenance routine, not
 * part of normal operation: the device boots straight into the UI using stored
 * calibration, and only runs this when explicitly asked.
 */

#pragma once

#include <TFT_eSPI.h>

#include "touch_input.h"

namespace touch {

/// Human-readable gesture name, for logging and diagnostics.
const char* gestureName(Gesture g);

/**
 * Derive calibration from three taps at known screen positions.
 *
 * Three targets rather than two, deliberately. See the implementation for why
 * a two-point diagonal calibration cannot detect a transposed digitizer — it
 * silently produced a mapping rotated 90 degrees.
 *
 * Blocks until the taps are given; there is no timeout, so calibration never
 * becomes a race against whoever is reading the on-screen prompts.
 *
 * @return false if the taps were inconsistent or too close together.
 */
bool runCalibration(TFT_eSPI& tft, TouchInput& input, Calibration& out);

/**
 * Ask for a swipe in a named direction and confirm the decoder agrees.
 *
 * Exists because an orientation fault is invisible to every value the firmware
 * can print, and previously surfaced only because a person noticed swipes going
 * the wrong way. This lets the device diagnose it itself.
 *
 * @return true if every direction checked matched.
 */
bool verifyOrientation(TFT_eSPI& tft, TouchInput& input);

}  // namespace touch
