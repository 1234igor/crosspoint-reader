#include <cassert>
#include <cstdint>

#include "util/HomeButtonInput.h"

int main() {
  using A = HomeButtonAction;
  HomeButtonInput input;
  auto tick = [&](uint32_t now, bool tap = false, bool hold = false, bool swipe = false, bool press = false) {
    return input.update(now, tap, hold, swipe, press, A::Home, A::ToggleFrontlight, A::ReaderMenu);
  };
  assert(tick(0, true) == A::Ignore);
  assert(tick(350) == A::Ignore);
  assert(tick(351) == A::Home);
  assert(tick(352) == A::Ignore);
  assert(tick(1000, true) == A::Ignore);
  assert(tick(1350, true) == A::ToggleFrontlight);
  assert(tick(1701) == A::Ignore);
  assert(tick(2000, false, true) == A::ReaderMenu);
  assert(tick(2001) == A::Ignore);
  // Tap then hold: the second contact prevents an early single-tap action.
  assert(tick(3000, true) == A::Ignore);
  assert(tick(3200, false, false, false, true) == A::Ignore);
  assert(tick(3400) == A::Ignore);
  assert(tick(3900, false, true) == A::ReaderMenu);
  assert(tick(4000) == A::Ignore);
  // A bezel swipe cancels the Home-key event and any deferred action.
  assert(tick(5000, true) == A::Ignore);
  assert(tick(5100, true, false, true) == A::Ignore);
  assert(tick(5500) == A::Ignore);
  assert(tick(6000, true) == A::Ignore);
  input.reset();
  assert(tick(6500) == A::Ignore);
  // Unsigned subtraction remains valid over the millisecond clock wrap.
  assert(tick(UINT32_MAX - 100, true) == A::Ignore);
  assert(tick(100, true) == A::ToggleFrontlight);
  assert(tick(UINT32_MAX - 100, true) == A::Ignore);
  assert(tick(251) == A::Home);
  // Two separated taps each produce a single action.
  assert(tick(7000, true) == A::Ignore);
  assert(tick(7400, true) == A::Home);
  assert(tick(7751) == A::Home);
  // Disabling double tap removes the single-tap delay.
  assert(input.update(8000, true, false, false, false, A::Bookmark, A::Ignore, A::ReaderMenu) == A::Bookmark);
}
