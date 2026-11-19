/*
 * Bottom key-hint bar ("Enter send  T text  R scan").
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_viewmodel.h"
#include "base_widget.h"

#include <string>
#include <utility>
#include <vector>

namespace app {
class AssetManager;
}

namespace view::widgets {

class HintBar : public BaseWidgets {
public:
    using Hints = std::vector<std::pair<std::string, std::string>>;  // key, action

    HintBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

    void build() override;
    void set_hints(const Hints& hints);

private:
    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    lv_obj_t* label_{nullptr};
};

} // namespace view::widgets
