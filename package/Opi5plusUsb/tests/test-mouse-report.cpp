// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of MouseReport.h: g++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I . tests/test-mouse-report.cpp -o /tmp/test-mouse-report && /tmp/test-mouse-report
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "MouseReport.h"

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
      failures++;                                                      \
    }                                                                  \
  } while (0)

using namespace opi5usb;

// The report descriptor of the Logitech 046d:c077 (and of most plain wheel mice): [buttons 3 bits + 5 pad][X][Y][wheel], no report ID.
static const uint8_t m105[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
                               0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
                               0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0};
// A boot mouse without a wheel: three buttons, X, Y.
static const uint8_t noWheel[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
                                  0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
                                  0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0};
// Report ID 1: five buttons + 3 pad, X and Y 16 bit, wheel 8 bit, horizontal wheel (Consumer AC Pan) 8 bit. Gaming-mouse style.
static const uint8_t withId[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25,
                                 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01,
                                 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81,
                                 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x81, 0x06, 0xc0, 0xc0};
// A first report ID without X and Y (vendor data), the mouse in the second.
static const uint8_t secondId[] = {0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x05, 0x09, 0x02, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0xc0,
                                   0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
                                   0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
                                   0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0};
// 12-bit X and Y (the two fields share three bytes): 3 buttons + 5 pad, X, Y, wheel.
static const uint8_t bits12[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
                                 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01,
                                 0xf8, 0x26, 0xff, 0x07, 0x75, 0x0c, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
                                 0x81, 0x06, 0xc0, 0xc0};
// A tablet-like pointer: X and Y absolute (Input flags 0x02), not a mouse this parser takes.
static const uint8_t absolute[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff,
                                   0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0, 0xc0};

int main() {
  MouseLayout l;

  CHECK(parseMouseLayout(m105, sizeof m105, &l));
  CHECK(l.valid && !l.hasId && l.length == 4);
  CHECK(l.buttons.bit == 0 && l.buttons.size == 3);
  CHECK(l.x.bit == 8 && l.x.size == 8 && l.x.sign);
  CHECK(l.y.bit == 16 && l.y.size == 8 && l.y.sign);
  CHECK(l.wheel.bit == 24 && l.wheel.size == 8 && l.wheel.sign);
  {
    const uint8_t r[4] = {0x05, 0xfe, 0x07, 0xff};   // buttons 1 and 3, X -2, Y 7, wheel -1
    CHECK(fieldValue(r, 4, l.buttons) == 5);
    CHECK(fieldValue(r, 4, l.x) == -2);
    CHECK(fieldValue(r, 4, l.y) == 7);
    CHECK(fieldValue(r, 4, l.wheel) == -1);
    const uint8_t up[4] = {0, 0, 0, 0x01};   // one notch away from the user
    CHECK(fieldValue(up, 4, l.wheel) == 1);
    CHECK(fieldValue(up, 3, l.wheel) == 0);   // a report cut short before the wheel byte: no wheel, no read past the end
  }

  l = MouseLayout();
  CHECK(parseMouseLayout(noWheel, sizeof noWheel, &l));
  CHECK(l.valid && l.length == 3 && l.wheel.bit == -1);
  {
    const uint8_t r[3] = {0, 1, 2};
    CHECK(fieldValue(r, 3, l.wheel) == 0);
  }

  l = MouseLayout();
  CHECK(parseMouseLayout(withId, sizeof withId, &l));
  CHECK(l.valid && l.hasId && l.reportId == 1 && l.length == 1 + 1 + 2 + 2 + 1 + 1);
  CHECK(l.buttons.bit == 0 && l.buttons.size == 3);   // only the first three of the five buttons are carried on
  CHECK(l.x.bit == 8 && l.x.size == 16 && l.y.bit == 24 && l.y.size == 16);
  CHECK(l.wheel.bit == 40 && l.wheel.size == 8);   // the Consumer AC Pan field after it is not the wheel
  {
    // data bytes after the ID: [buttons][X lo][X hi][Y lo][Y hi][wheel][pan]
    const uint8_t d[6] = {0x01, 0x34, 0xff, 0x10, 0x00, 0xfe};   // X 0xff34 = -204, Y 16, wheel -2
    CHECK(fieldValue(d, 6, l.x) == -204);
    CHECK(fieldValue(d, 6, l.y) == 16);
    CHECK(fieldValue(d, 6, l.wheel) == -2);
  }

  l = MouseLayout();
  CHECK(parseMouseLayout(secondId, sizeof secondId, &l));
  CHECK(l.hasId && l.reportId == 2 && l.length == 5 && l.wheel.bit == 24);

  l = MouseLayout();
  CHECK(parseMouseLayout(bits12, sizeof bits12, &l));
  CHECK(l.x.bit == 8 && l.x.size == 12 && l.y.bit == 20 && l.y.size == 12 && l.wheel.bit == 32 && l.length == 5);
  {
    // bytes after the buttons: ff f3 ff. X = bits 8..19 of the report = ff and the low nibble of f3 = 0x3ff (positive: bit 11 is 0);
    // Y = bits 20..31 = the high nibble of f3 and ff = 0xfff = -1
    const uint8_t d[5] = {0x00, 0xff, 0xf3, 0xff, 0x00};
    CHECK(fieldValue(d, 5, l.x) == 0x3ff);
    CHECK(fieldValue(d, 5, l.y) == -1);
    const uint8_t e[5] = {0x00, 0x00, 0x08, 0x00, 0x00};   // X = 0x800 = -2048
    CHECK(fieldValue(e, 5, l.x) == -2048);
  }

  l = MouseLayout();
  CHECK(!parseMouseLayout(absolute, sizeof absolute, &l) && !l.valid);

  // truncated and meaningless input never reads past the end and never says "valid" for a mouse that is not there
  l = MouseLayout();
  for (unsigned n = 0; n < sizeof m105; ++n) {
    MouseLayout t;
    const bool ok = parseMouseLayout(m105, n, &t);
    CHECK(!ok || t.valid);
    if (n < 40) CHECK(!ok);   // X and Y are not complete before byte 46
  }
  CHECK(!parseMouseLayout(m105, 0, &l));
  srand(1);
  for (int i = 0; i < 20000; ++i) {
    uint8_t junk[64];
    const unsigned n = rand() % 64;
    for (unsigned k = 0; k < n; ++k) junk[k] = static_cast<uint8_t>(rand());
    MouseLayout t;
    if (parseMouseLayout(junk, n, &t)) CHECK(t.valid && t.x.bit >= 0 && t.y.bit >= 0);
  }

  printf(failures ? "FAILED: %d\n" : "mouse report parser: all checks passed\n", failures);
  return failures ? 1 : 0;
}
