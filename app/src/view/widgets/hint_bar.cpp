/*
 * Bottom key-hint bar.
 * SPDX-License-Identifier: MIT
 */

#include "hint_bar.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"

#include <cstdio>

namespace view::widgets {

HintBar::HintBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseWidgets(parent), view_model_(view_model), assets_(assets) {}

void HintBar::build() {
    if (core_obj_) {
        return;
    }
    core_obj_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(core_obj_);
    lv_obj_set_size(core_obj_, LV_PCT(100), kNavBarHeight);
    lv_obj_align(core_obj_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(core_obj_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(core_obj_, view_model_.dark_mode_subject(), reactive::ThemeRole::Bar);

    label_ = lv_label_create(core_obj_);
    lv_label_set_recolor(label_, true);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label_, kScreenWidth - 12);
    const lv_font_t* font = assets_.load_standard_font(12, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(label_, font ? font : &lv_font_montserrat_12, 0);
    lv_obj_align(label_, LV_ALIGN_LEFT_MID, 6, 0);
    reactive::bind_theme(label_, view_model_.dark_mode_subject(), reactive::ThemeRole::Text);
}

void HintBar::set_hints(const Hints& hints) {
    if (!label_) {
        return;
    }
    const auto colors = palette(view_model_.is_dark_mode());
    const uint32_t key_color = lv_color_to_u32(colors.primary) & 0xFFFFFF;
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%06X", key_color);

    std::string text;
    for (const auto& hint : hints) {
        if (!text.empty()) {
            text += "   ";
        }
        text += "#";
        text += hex;
        text += " ";
        text += hint.first;
        text += "# ";
        text += hint.second;
    }
    lv_label_set_text(label_, text.c_str());
}

} // namespace view::widgets
