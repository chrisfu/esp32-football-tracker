/**
 * @file crest_cache.h
 * @brief Team crests: fetched once, decoded on-device, cached as raw pixels.
 *
 * **Only the crests currently in use are kept.** With the team chosen by the
 * user, caching a whole division makes no sense — the interesting set is tiny
 * and changes slowly:
 *
 *   - our own team
 *   - whoever we last played
 *   - whoever we play next
 *   - whoever we are playing right now, if a match is on
 *
 * That is at most four, and usually three. Anything else is pruned, so the
 * cache stays a few kilobytes regardless of which league the user follows.
 *
 * Crests cost **no API quota**: crests.football-data.org is a plain static
 * host, not the rate-limited API. It does still cost a TLS handshake and a
 * decode, so each crest is fetched exactly once and then read from flash.
 *
 * Stored as raw RGB565 with alpha already composited against the UI
 * background, so drawing is a straight copy — no decode, no blending, and no
 * decoder in the drawing path at all.
 */

#pragma once

#include <TFT_eSPI.h>
#include <stdint.h>

namespace crest {

/// Stored crest size. 48x48 is 4,608 bytes each, so even four is ~18 KB.
constexpr int16_t kSize = 48;

/// Bring up the crest directory.
void begin();

/**
 * Make sure a crest is cached, fetching and decoding it if not.
 *
 * Blocking, and slow — a TLS handshake plus a PNG decode — so it is called
 * only from the fetch task, never from the UI.
 *
 * @return true if the crest is now available on disk.
 */
bool ensure(uint16_t teamId);

/// Whether a crest is already on disk and readable.
bool available(uint16_t teamId);

/**
 * Delete every cached crest not in `keep`.
 *
 * Called after the fixtures change, which is when the interesting set moves.
 */
void prune(const uint16_t* keep, uint8_t count);

/**
 * Draw a cached crest.
 *
 * Reads the file a row at a time into a 96-byte buffer rather than loading
 * the whole 4.6 KB image, because the UI has no reason to hold a buffer it
 * uses once per redraw.
 *
 * @return false if the crest is not cached, so the caller can fall back to
 *         text without a gap in the layout.
 */
bool draw(TFT_eSPI& tft, uint16_t teamId, int16_t x, int16_t y);

/// Half the stored size, for screens where a full crest costs content.
constexpr int16_t kHalfSize = kSize / 2;

/**
 * Draw a cached crest at half size, box-averaging 2x2 blocks as it reads.
 *
 * Averaged rather than sampled: dropping every other pixel of a crest — which
 * is mostly lettering and thin heraldic detail — breaks strokes up visibly,
 * and the averaging costs one extra row buffer and some adds.
 *
 * No second cached size is kept. At 24x24 the saving would be 1.1 KB per
 * crest against the cost of a second decode path and a second thing to keep
 * consistent.
 */
bool drawHalf(TFT_eSPI& tft, uint16_t teamId, int16_t x, int16_t y);

/// Bytes currently used by cached crests, for the web UI.
uint32_t bytesUsed();

}  // namespace crest
