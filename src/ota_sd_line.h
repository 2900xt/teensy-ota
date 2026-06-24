/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Block-buffered text-line reader over an open SD file, shared by the bootloader
 * commit path and the app-side inspect API. Reading a hex file byte-by-byte
 * through SdFat's cache is slow; this refills a 512-byte buffer and hands out
 * lines. Header-only so both the library and the bootloader can use it.
 *
 * Templated on the file type (anything exposing `int read(void*, size_t)`) so it
 * works equally with the Arduino `SD` wrapper's `File` (bootloader) and SdFat's
 * `FsFile` opened off an application-owned mount (app side). Instantiate
 * explicitly at the call site, e.g. `OtaFileReader<FsFile> rd(f);`.
 */
#ifndef TEENSY_OTA_SD_LINE_H
#define TEENSY_OTA_SD_LINE_H

#include <stddef.h>
#include <stdint.h>

template <class FileT> class OtaFileReader {
 public:
      explicit OtaFileReader(FileT& f) : f_(f) {}

      // Next byte, or -1 at EOF.
      int next() {
            if (i_ >= n_) {
                  const int r = f_.read(buf_, sizeof(buf_));
                  if (r <= 0) return -1;
                  n_ = r;
                  i_ = 0;
            }
            return buf_[i_++];
      }

      // Read one line (up to but excluding '\n') into `out`, stripping a trailing
      // '\r'. Returns the length, 0 for a blank line, -1 at clean EOF, or -2 if
      // the line would overflow `cap`.
      int line(char* out, int cap) {
            int len = 0;
            int c = next();
            if (c < 0) return -1;
            while (c >= 0 && c != '\n') {
                  if (len >= cap - 1) return -2;
                  out[len++] = (char)c;
                  c = next();
            }
            if (len > 0 && out[len - 1] == '\r') len--;
            out[len] = '\0';
            return len;
      }

 private:
      FileT& f_;
      uint8_t buf_[512];
      int n_ = 0;
      int i_ = 0;
};

#endif // TEENSY_OTA_SD_LINE_H
