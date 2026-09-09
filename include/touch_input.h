/**
 * @file touch_input.h
 * @brief XPT2046 resistive touch input with calibration and gesture decoding.
 *
 * The XPT2046 sits on its own SPI pins (see board_config.h), separate from both
 * the display and the SD card, so touch reads never contend for a chip select
 * with a screen redraw.
 *
 * The controller is driven directly rather than through a library: the protocol
 * is a handful of register reads, and doing it ourselves means press detection
 * can use the hardware IRQ line — a single digital read — instead of
 * continuously polling over SPI. That is both cheaper per poll and the same
 * mechanism that wakes the device from deep sleep.
 *
 * Three things this class adds over raw register reads:
 *
 *   - **Calibration.** A resistive panel's raw ADC range varies between units
 *     and does not map linearly onto screen pixels without per-device
 *     correction. Calibration constants are held here and are meant to be
 *     persisted to NVS once configuration storage exists.
 *   - **Debouncing and gesture decoding.** Raw resistive readings are noisy and
 *     produce spurious touches at the moment of press and release. The UI wants
 *     "tap" and "swipe left/right", not a stream of jittering coordinates.
 */

#pragma once

#include <SPI.h>
#include <stdint.h>

#include "board_config.h"

namespace touch {

/**
 * Linear mapping from raw controller ADC values to screen pixels.
 *
 * Derived by tapping two known targets — see TouchInput::calibrate(). The
 * defaults are a reasonable starting point for this board, but every unit
 * should be calibrated; they exist so the UI is usable before calibration has
 * been run.
 */
struct Calibration {
  // Signed, and deliberately so. These are the extrapolated raw values at the
  // screen edges, and a correct linear fit can legitimately land outside the
  // controller's 0..4095 output range — calibration on this unit produced a
  // rawMinY of -41. Clamping that to an unsigned 0 compressed the top of the
  // screen by ~2.5 px, so the fit is stored as measured instead.
  // NOTE ON NAMING: these bound the raw channel *assigned to* that screen
  // axis, which is not the same as the controller channel of the same letter.
  // With swapAxes set, rawMinX/rawMaxX bound the controller's Y channel. They
  // are named for the screen axis they produce, because that is what every
  // caller cares about.
  int16_t rawMinX = 195;
  int16_t rawMaxX = 3711;
  int16_t rawMinY = 347;
  int16_t rawMaxY = 3682;
  /**
   * True when the controller's X axis corresponds to the screen's Y axis.
   *
   * The digitizer reads in the panel's native portrait orientation while the
   * display runs landscape, so on this board the axes are transposed. This is
   * *detected* during calibration, never assumed — and detecting it requires a
   * calibration target off the diagonal, which is why there are three.
   */
  bool swapAxes = true;  // Measured true on the reference unit.

  /// Some panels are wired with an axis reversed relative to the display.
  /// Detected during calibration rather than assumed.
  bool invertX = false;
  bool invertY = false;
  /// Set once real calibration has been applied, so the UI can prompt if not.
  bool valid = false;
};

// The defaults above are the reference unit's measured three-point fit, and
// unlike the earlier two-point figures they have been verified by a swipe
// direction check on hardware. They are still no substitute for calibrating
// each board; once NVS config exists, an uncalibrated device will offer
// calibration on first boot rather than assume these apply.

/// A decoded gesture. The screen manager consumes these rather than raw points.
enum class Gesture : uint8_t {
  None,
  Tap,         ///< Press and release without significant movement.
  SwipeLeft,   ///< Next screen.
  SwipeRight,  ///< Previous screen.
  SwipeUp,     ///< Scroll down within a screen (e.g. the league table).
  SwipeDown,   ///< Scroll up within a screen.
};

class TouchInput {
 public:
  /// Bring up the touch controller on its dedicated SPI bus.
  void begin();

  /// True while the panel is being pressed, after debouncing.
  bool isPressed();

  /**
   * Latest touch position in screen pixels.
   * @return false if nothing is currently being touched, leaving x/y untouched.
   */
  bool getPoint(int16_t& x, int16_t& y);

  /// Raw, uncalibrated controller values. Used by calibration and diagnostics.
  bool getRaw(uint16_t& x, uint16_t& y, uint16_t& z);

  /**
   * Unconditional raw read that bypasses both the IRQ check and the pressure
   * gate. Diagnostics only.
   *
   * Exists so bring-up can distinguish "the panel was not touched" from "the
   * SPI link is broken" — the filtered read returns false in both cases, which
   * makes it useless for telling them apart.
   */
  void readRawUnfiltered(uint16_t& x, uint16_t& y, uint16_t& z);

  /// Pressure below which contact is treated as noise. Exposed so bring-up can
  /// report it alongside observed values when tuning.
  static uint16_t pressureThreshold();

  /**
   * Poll for a completed gesture.
   *
   * Must be called regularly; gestures are decoded from the press/release
   * cycle, so a caller that polls sporadically will miss them. Returns
   * Gesture::None most of the time.
   */
  Gesture poll();

  const Calibration& calibration() const { return calibration_; }
  void setCalibration(const Calibration& c) { calibration_ = c; }

  /**
   * True if the touch IRQ line is asserted.
   *
   * Exposed because this same line is the deep-sleep wake source: if it does
   * not behave here, wake-on-touch will not work either.
   */
  bool irqAsserted() const;

 private:
  int16_t mapRawToScreenX(uint16_t rawX, uint16_t rawY) const;
  int16_t mapRawToScreenY(uint16_t rawX, uint16_t rawY) const;

  /// Read one 12-bit channel from the controller.
  /// @param command XPT2046 control byte (see the constants in the .cpp).
  uint16_t readChannel(uint8_t command);

  /// Median-of-three sample of the position channels, to reject spikes.
  bool sampleRaw(uint16_t& x, uint16_t& y, uint16_t& z);

  /// Dedicated SPI bus instance for the touch controller.
  SPIClass spi_{VSPI};
  Calibration calibration_{};

  // --- Gesture decoding state ---
  bool     tracking_    = false;
  int16_t  startX_      = 0;
  int16_t  startY_      = 0;
  int16_t  lastX_       = 0;
  int16_t  lastY_       = 0;
  uint32_t pressStart_  = 0;
  uint32_t lastRelease_ = 0;

  /// Movement below this is a tap, not a swipe (pixels).
  static constexpr int16_t kTapSlop = 24;
  /// Movement above this along an axis counts as a swipe (pixels).
  static constexpr int16_t kSwipeMin = 55;
  /// Presses longer than this are ignored as gestures (a resting finger).
  static constexpr uint32_t kMaxGestureMs = 1200;
  /// Ignore re-presses within this window to absorb release chatter.
  static constexpr uint32_t kDebounceMs = 45;
};

}  // namespace touch
