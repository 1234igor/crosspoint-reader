#pragma once

#include "HomeButtonSettings.h"
#include "activities/UiListActivity.h"

class HomeButtonSettingsActivity final : public UiListActivity {
 public:
  HomeButtonSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("HomeButtonSettings", renderer, input) {}

 private:
  int gesture = -1;
  freeink::ui::ListItem rows[static_cast<unsigned>(HomeButtonAction::Count)]{};
  int listCount() const override { return gesture < 0 ? 3 : static_cast<int>(HomeButtonAction::Count); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
};
