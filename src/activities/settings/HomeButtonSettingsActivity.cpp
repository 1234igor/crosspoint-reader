#include "HomeButtonSettingsActivity.h"

#include "components/UITheme.h"

namespace fui = freeink::ui;

const char* HomeButtonSettingsActivity::headerTitle() const {
  return gesture < 0 ? tr(STR_HOME_BUTTON) : I18N.get(home_button::GESTURE_LABELS[gesture]);
}

void HomeButtonSettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount()) return;
  mappedInput.resetHomeButtonInput();
  app.clearTapFlash();
  if (gesture < 0) {
    {
      RenderLock lock(*this);
      gesture = index;
      nav = {};
      nav.selected = SETTINGS.*home_button::FIELDS[gesture];
      nav.follow(listCount());
    }
    resetUi();
    requestUpdate();
    return;
  }
  auto& value = SETTINGS.*home_button::FIELDS[gesture];
  if (value != index) {
    value = static_cast<uint8_t>(index);
    SETTINGS.saveToFile();
  }
  onBackButton();
}

void HomeButtonSettingsActivity::onBackButton() {
  if (gesture < 0) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    const int previous = gesture;
    gesture = -1;
    nav = {};
    nav.selected = previous;
    nav.follow(listCount());
  }
  resetUi();
  requestUpdate();
}

void HomeButtonSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  for (int i = 0; i < listCount(); ++i) {
    rows[i].label = I18N.get(gesture < 0 ? home_button::GESTURE_LABELS[i] : home_button::ACTION_LABELS[i]);
    rows[i].actionValue = static_cast<int16_t>(i);
    const uint8_t value = gesture < 0 ? SETTINGS.*home_button::FIELDS[i] : 0;
    rows[i].value = gesture < 0 && value < static_cast<uint8_t>(HomeButtonAction::Count)
                        ? I18N.get(home_button::ACTION_LABELS[value])
                        : nullptr;
  }
  fui::ListProps props;
  props.items = rows;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  if (gesture < 0) {
    // Keep the gesture name and its current action at the same visual weight.
    props.labelText = screen.theme().smallText;
    // A default smallText style is treated as inherited by screen.list().
    props.labelText.maxLines = 2;
  }
  syncListViewport(screen, props);
  screen.list(props);
}
