/**
 * @file crest_cache.cpp
 * @brief Crest fetching, decoding and caching. See crest_cache.h.
 */

#include "crest_cache.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <PNGdec.h>
#include <WiFiClientSecure.h>
#include <sys/stat.h>
#include <unistd.h>

#include "assets_root_certs.h"
#include "screen.h"

namespace crest {
namespace {

constexpr const char* kHost    = "crests.football-data.org";
constexpr const char* kDir     = "/crests";
constexpr const char* kFsBase  = "/littlefs";

/// Bytes per stored crest.
constexpr size_t kStoredBytes = static_cast<size_t>(kSize) * kSize * 2;

/// Upper bound on a source PNG. The observed crest is 7,736 bytes; 32 KB is
/// generous while still refusing anything that would threaten the heap.
constexpr size_t kMaxPngBytes = 32UL * 1024UL;

constexpr uint32_t kFetchTimeoutMs = 15000;

// --- Decode state -----------------------------------------------------------
//
// PNGdec calls back per line, so the scaling accumulators have to outlive the
// callback. File-local rather than passed through, because PNGdec's callback
// signature carries no user pointer.

/**
 * The decoder, allocated only while decoding.
 *
 * PNGdec's PNG object embeds its inflate window, so a static instance costs
 * around 46 KB of RAM permanently — measured as a jump from 16.6% to 30.9% of
 * internal DRAM when it was a plain member. That is a poor trade for something
 * used once per crest and never again, especially with TLS wanting ~42 KB of
 * heap at the same time. Allocated on demand and released immediately.
 */
PNG*     g_png = nullptr;
uint32_t g_accR[kSize];   ///< Colour sums per output column.
uint32_t g_accG[kSize];
uint32_t g_accB[kSize];
uint32_t g_accN[kSize];   ///< Source pixels contributing to each column.
uint16_t g_outRow[kSize];
File     g_outFile;
int16_t  g_srcWidth  = 0;
int16_t  g_srcHeight = 0;
int16_t  g_currentOutRow = -1;
bool     g_writeFailed   = false;
uint16_t g_linesSeen     = 0;  ///< Callback invocations, for diagnostics.
/**
 * Bytes actually written to the output file.
 *
 * Counted here rather than read back from File::size(), which reports 0 for a
 * LittleFS file still open for writing — so a decode that had written every
 * row still looked as though it had produced nothing.
 */
uint32_t g_bytesWritten  = 0;

/**
 * One decoded source line, allocated to the image's actual width.
 *
 * Crests are *not* a uniform size: Bolton's is 70x70 while West Ham's and
 * Cardiff's are 200x200. A fixed buffer sized from the first crest inspected
 * silently failed on every larger one, so the width comes from the file.
 */
uint16_t* g_lineBuf   = nullptr;
int16_t   g_lineWidth = 0;

/// Refuse anything absurd rather than allocating it. No crest is this wide,
/// and it bounds the allocation regardless of what the server sends.
constexpr int16_t kMaxSourceWidth = 512;

void resetAccumulators() {
  memset(g_accR, 0, sizeof(g_accR));
  memset(g_accG, 0, sizeof(g_accG));
  memset(g_accB, 0, sizeof(g_accB));
  memset(g_accN, 0, sizeof(g_accN));
}

/// Emit the accumulated output row to the file.
void flushOutputRow() {
  if (g_currentOutRow < 0 || g_writeFailed) return;
  for (int16_t x = 0; x < kSize; ++x) {
    const uint32_t n = g_accN[x] ? g_accN[x] : 1;
    const uint8_t r = static_cast<uint8_t>(g_accR[x] / n);
    const uint8_t g = static_cast<uint8_t>(g_accG[x] / n);
    const uint8_t b = static_cast<uint8_t>(g_accB[x] / n);
    // RGB565, high byte first — the order pushImage expects with the default
    // byte swap on this display.
    g_outRow[x] = static_cast<uint16_t>(((r & 0xF8) << 8) |
                                        ((g & 0xFC) << 3) | (b >> 3));
  }
  const size_t written = g_outFile.write(
      reinterpret_cast<const uint8_t*>(g_outRow), sizeof(g_outRow));
  if (written != sizeof(g_outRow)) {
    g_writeFailed = true;
  } else {
    g_bytesWritten += written;
  }
  resetAccumulators();
}

/**
 * PNGdec line callback: box-filter this source line into the output grid.
 *
 * Area averaging rather than nearest-neighbour. 70 to 48 is a modest
 * reduction, but a crest is fine detail and mostly text — nearest-neighbour
 * on lettering looks visibly broken, and averaging costs only a few hundred
 * bytes of accumulators.
 */
/**
 * PNGdec line callback.
 *
 * **Returns 1 to continue and 0 to abort** — PNGdec treats a zero return as
 * "quit early" (`if (!(*pfnDraw)(&pngd)) { iError = PNG_QUIT_EARLY; }`).
 * Getting this backwards produced a decode that stopped after exactly one
 * line and reported error 8, which is easy to mistake for a corrupt file.
 */
int onLine(PNGDRAW* pDraw) {
  if (g_writeFailed || g_png == nullptr) return 0;      // Abort.
  if (g_lineBuf == nullptr || pDraw->iWidth > g_lineWidth) {
    g_writeFailed = true;
    return 0;                                           // Abort.
  }
  if (g_srcHeight <= 0 || g_srcWidth <= 0) return 0;     // Abort.

  // Alpha is composited here, against the UI background, so nothing
  // downstream ever has to blend. That is the whole reason the stored form is
  // opaque RGB565.
  uint16_t* lineBuf = g_lineBuf;
  g_png->getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_BIG_ENDIAN,
                         ui::colour::kBackground);

  ++g_linesSeen;
  const int16_t outRow = static_cast<int16_t>(
      (static_cast<int32_t>(pDraw->y) * kSize) / g_srcHeight);

  // A new output row means the previous one is complete.
  if (outRow != g_currentOutRow) {
    flushOutputRow();
    g_currentOutRow = outRow;
  }

  for (int16_t x = 0; x < pDraw->iWidth; ++x) {
    const int16_t outCol =
        static_cast<int16_t>((static_cast<int32_t>(x) * kSize) / g_srcWidth);
    if (outCol < 0 || outCol >= kSize) continue;
    const uint16_t p = lineBuf[x];
    // Expand RGB565 back to 8-bit per channel for averaging.
    g_accR[outCol] += ((p >> 11) & 0x1F) << 3;
    g_accG[outCol] += ((p >> 5) & 0x3F) << 2;
    g_accB[outCol] += (p & 0x1F) << 3;
    ++g_accN[outCol];
  }
  return 1;  // Continue decoding.
}

void crestPath(uint16_t teamId, char* out, size_t len) {
  snprintf(out, len, "%s/%u.raw", kDir, teamId);
}

bool statQuietly(const char* path, uint32_t& sizeOut) {
  char full[80];
  snprintf(full, sizeof(full), "%s%s", kFsBase, path);
  struct stat st;
  if (::stat(full, &st) != 0) return false;
  sizeOut = static_cast<uint32_t>(st.st_size);
  return true;
}

void removeQuietly(const char* path) {
  char full[80];
  snprintf(full, sizeof(full), "%s%s", kFsBase, path);
  ::unlink(full);
}

/// Download a crest PNG into a freshly allocated buffer.
/// @return byte count, or 0 on failure. Caller frees.
size_t download(uint16_t teamId, uint8_t** buffer) {
  WiFiClientSecure client;
  client.setCACert(assets::kRootCaBundle);
  client.setTimeout(5);

  if (!client.connect(kHost, 443)) {
    Serial.printf("[crest] TLS connect failed for %u\n", teamId);
    return 0;
  }

  client.printf(
      "GET /%u.png HTTP/1.1\r\nHost: %s\r\n"
      "User-Agent: ESP32-FootballTracker/1.0\r\nConnection: close\r\n\r\n",
      teamId, kHost);

  const uint32_t deadline = millis() + kFetchTimeoutMs;

  // Headers. Content-Length is used when present, which it is on this host —
  // it means the buffer is sized exactly rather than grown.
  size_t contentLength = 0;
  uint16_t status = 0;
  bool firstLine = true;
  char line[192];
  size_t len = 0;
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(2);
      continue;
    }
    const int c = client.read();
    if (c < 0) continue;
    if (c == '\n') {
      while (len > 0 && line[len - 1] == '\r') --len;
      line[len] = '\0';
      if (firstLine) {
        const char* sp = strchr(line, ' ');
        status = (sp != nullptr) ? atoi(sp + 1) : 0;
        firstLine = false;
      } else if (line[0] == '\0') {
        break;  // End of headers.
      } else if (strncasecmp(line, "content-length:", 15) == 0) {
        contentLength = static_cast<size_t>(atol(line + 15));
      }
      len = 0;
      continue;
    }
    if (len < sizeof(line) - 1) line[len++] = static_cast<char>(c);
  }

  if (status != 200 || contentLength == 0 || contentLength > kMaxPngBytes) {
    Serial.printf("[crest] %u: status %u, length %u -- giving up\n", teamId,
                  status, (unsigned)contentLength);
    client.stop();
    return 0;
  }

  uint8_t* buf = static_cast<uint8_t*>(malloc(contentLength));
  if (buf == nullptr) {
    Serial.printf("[crest] cannot allocate %u bytes\n",
                  (unsigned)contentLength);
    client.stop();
    return 0;
  }

  size_t got = 0;
  while (got < contentLength && millis() < deadline) {
    const int avail = client.available();
    if (avail > 0) {
      const int n = client.read(buf + got, min(static_cast<size_t>(avail),
                                               contentLength - got));
      if (n > 0) {
        got += static_cast<size_t>(n);
        continue;
      }
    }
    if (!client.connected() && client.available() == 0) break;
    vTaskDelay(1);  // Yield, for the same reason the API client does.
  }
  client.stop();

  if (got != contentLength) {
    Serial.printf("[crest] %u: short read (%u of %u)\n", teamId, (unsigned)got,
                  (unsigned)contentLength);
    free(buf);
    return 0;
  }
  *buffer = buf;
  return got;
}

}  // namespace

void begin() {
  uint32_t ignored = 0;
  if (!statQuietly(kDir, ignored)) LittleFS.mkdir(kDir);
}

bool available(uint16_t teamId) {
  char path[40];
  crestPath(teamId, path, sizeof(path));
  uint32_t size = 0;
  // A wrong size means a partial write from an interrupted decode; treat it
  // as absent so it is re-fetched rather than drawn as garbage.
  return statQuietly(path, size) && size == kStoredBytes;
}

bool ensure(uint16_t teamId) {
  if (teamId == 0) return false;
  if (available(teamId)) return true;

  uint8_t* png = nullptr;
  const size_t pngBytes = download(teamId, &png);
  if (pngBytes == 0) return false;

  char path[40], tmp[44];
  crestPath(teamId, path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s/%u.tmp", kDir, teamId);

  // Written to a temporary and renamed, for the same reason the JSON cache is:
  // a reset partway through a decode must not leave a half-image that would
  // then be drawn as noise.
  g_outFile = LittleFS.open(tmp, "w");
  if (!g_outFile) {
    free(png);
    return false;
  }

  g_currentOutRow = -1;
  g_writeFailed   = false;
  g_linesSeen     = 0;
  g_bytesWritten  = 0;
  resetAccumulators();

  // Verify it really is a PNG before handing it to the decoder, and log the
  // header, so a server returning something unexpected is distinguishable
  // from a decoder problem.
  const bool looksPng = pngBytes > 8 && png[0] == 0x89 && png[1] == 'P' &&
                        png[2] == 'N' && png[3] == 'G';
  if (!looksPng) {
    // Logged only when wrong, so a server returning an error page or a
    // redirect body is distinguishable from a decoder fault.
    Serial.printf("[crest] %u: not a PNG (%02X%02X%02X%02X, %u bytes)\n",
                  teamId, png[0], png[1], png[2], png[3], (unsigned)pngBytes);
    free(png);
    return false;
  }

  bool ok = false;
  g_png = new PNG();
  if (g_png == nullptr) {
    Serial.println(F("[crest] cannot allocate decoder"));
    g_outFile.close();
    removeQuietly(tmp);
    free(png);
    return false;
  }

  const int openRc = g_png->openRAM(png, pngBytes, onLine);
  if (openRc == PNG_SUCCESS) {
    g_srcWidth  = g_png->getWidth();
    g_srcHeight = g_png->getHeight();
    if (g_srcWidth > 0 && g_srcWidth <= kMaxSourceWidth && g_srcHeight > 0) {
      g_lineWidth = g_srcWidth;
      g_lineBuf   = static_cast<uint16_t*>(
          malloc(static_cast<size_t>(g_lineWidth) * sizeof(uint16_t)));
    }
    if (g_lineBuf != nullptr) {
      const int rc = g_png->decode(nullptr, 0);
      ok = (rc == PNG_SUCCESS);
      if (!ok) {
        Serial.printf("[crest] %u: decode failed rc=%d after %u lines\n",
                      teamId, rc, g_linesSeen);
      }
      flushOutputRow();  // The final row is still in the accumulators.
      free(g_lineBuf);
      g_lineBuf   = nullptr;
      g_lineWidth = 0;
    } else {
      Serial.printf("[crest] %u: unusable source %dx%d\n", teamId, g_srcWidth,
                    g_srcHeight);
    }
    g_png->close();
  } else {
    Serial.printf("[crest] %u: openRAM rc=%d\n", teamId, openRc);
  }
  delete g_png;
  g_png = nullptr;

  g_outFile.flush();
  const size_t produced = g_bytesWritten;
  g_outFile.close();
  free(png);

  // Every row must have been written; a partial image is not usable.
  if (!ok || g_writeFailed || produced != kStoredBytes) {
    Serial.printf("[crest] %u: decode produced %u of %u bytes -- discarding\n",
                  teamId, (unsigned)produced, (unsigned)kStoredBytes);
    removeQuietly(tmp);
    return false;
  }

  removeQuietly(path);
  if (!LittleFS.rename(tmp, path)) {
    removeQuietly(tmp);
    return false;
  }
  Serial.printf("[crest] %u cached (%dx%d source -> %dx%d, %u bytes)\n",
                teamId, g_srcWidth, g_srcHeight, kSize, kSize,
                (unsigned)kStoredBytes);
  return true;
}

void prune(const uint16_t* keep, uint8_t count) {
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return;

  // Names are collected before deleting: removing entries while iterating a
  // directory is asking for trouble.
  constexpr uint8_t kMaxDoomed = 12;
  char doomed[kMaxDoomed][40];
  uint8_t doomedCount = 0;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* name = f.name();
    if (name == nullptr) continue;
    const uint16_t id = static_cast<uint16_t>(atoi(name));
    bool wanted = false;
    for (uint8_t i = 0; i < count; ++i) {
      if (keep[i] == id) {
        wanted = true;
        break;
      }
    }
    if (!wanted && doomedCount < kMaxDoomed) {
      snprintf(doomed[doomedCount], sizeof(doomed[0]), "%s/%s", kDir, name);
      ++doomedCount;
    }
  }
  dir.close();

  for (uint8_t i = 0; i < doomedCount; ++i) {
    removeQuietly(doomed[i]);
    Serial.printf("[crest] pruned %s\n", doomed[i]);
  }
}

bool draw(TFT_eSPI& tft, uint16_t teamId, int16_t x, int16_t y) {
  if (teamId == 0) return false;
  // Checked with a silent stat before opening. LittleFS.open() logs at ERROR
  // level for a missing file, and this is called on every redraw — the Next
  // screen's countdown refreshes once a second, so a crest that has not
  // downloaded yet produced two spurious error lines per second.
  if (!available(teamId)) return false;

  char path[40];
  crestPath(teamId, path, sizeof(path));

  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() != kStoredBytes) {
    f.close();
    return false;
  }

  // One row at a time: 96 bytes rather than the whole 4.6 KB image. The UI has
  // no reason to hold a buffer it uses once per redraw.
  uint16_t row[kSize];
  for (int16_t r = 0; r < kSize; ++r) {
    if (f.read(reinterpret_cast<uint8_t*>(row), sizeof(row)) !=
        static_cast<int>(sizeof(row))) {
      f.close();
      return false;
    }
    tft.pushImage(x, y + r, kSize, 1, row);
  }
  f.close();
  return true;
}

bool drawHalf(TFT_eSPI& tft, uint16_t teamId, int16_t x, int16_t y) {
  if (teamId == 0 || !available(teamId)) return false;

  char path[40];
  crestPath(teamId, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() != kStoredBytes) {
    f.close();
    return false;
  }

  uint16_t rowA[kSize], rowB[kSize], out[kHalfSize];
  for (int16_t r = 0; r < kHalfSize; ++r) {
    // Two source rows per output row, so each output pixel averages a 2x2
    // block rather than picking one corner of it.
    if (f.read(reinterpret_cast<uint8_t*>(rowA), sizeof(rowA)) !=
            static_cast<int>(sizeof(rowA)) ||
        f.read(reinterpret_cast<uint8_t*>(rowB), sizeof(rowB)) !=
            static_cast<int>(sizeof(rowB))) {
      f.close();
      return false;
    }
    for (int16_t o = 0; o < kHalfSize; ++o) {
      const uint16_t p[4] = {rowA[o * 2], rowA[o * 2 + 1], rowB[o * 2],
                             rowB[o * 2 + 1]};
      uint16_t rr = 0, gg = 0, bb = 0;
      for (uint16_t v : p) {
        rr += (v >> 11) & 0x1F;
        gg += (v >> 5) & 0x3F;
        bb += v & 0x1F;
      }
      out[o] = static_cast<uint16_t>(((rr / 4) << 11) | ((gg / 4) << 5) |
                                     (bb / 4));
    }
    tft.pushImage(x, y + r, kHalfSize, 1, out);
  }
  f.close();
  return true;
}

uint32_t bytesUsed() {
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return 0;
  uint32_t total = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    total += f.size();
  }
  dir.close();
  return total;
}

}  // namespace crest
