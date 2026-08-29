#pragma once

// Kept in a header because the Arduino IDE inserts auto-generated function
// prototypes at the very top of the .ino, above any struct defined there.
// Headers are left alone, so the type exists before those prototypes.

struct Control {
  const char *openIn;      // YES / NO
  const char *openOut;     // YES / NO
  const char *arrowIn;     // YES / NO
  uint32_t    holdMs;      // how long to hold this state before reverting
};
