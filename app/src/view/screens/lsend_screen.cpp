/*
 * LocalSend main screen.
 * SPDX-License-Identifier: MIT
 */

#include "lsend_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace screen {
namespace {

// Phosphor-Fill codepoints (resolved from the bundled font's ligature table)
constexpr const char* kIconFolder = "";
constexpr const char* kIconFile = "";
constexpr const char* kIconFileText = "";
constexpr const char* kIconImage = "";
constexpr const char* kIconMusic = "";
constexpr const char* kIconFilm = "";
constexpr const char* kIconMobile = "";
constexpr const char* kIconDesktop = "";
constexpr const char* kIconLaptop = "";
constexpr const char* kIconTablet = "";
constexpr const char* kIconTerminal = "";
constexpr const char* kIconGlobe = "";
constexpr const char* kIconTray = "";
constexpr const char* kIconChat = "";
constexpr const char* kIconUp = "";

constexpr int kContentHeight = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
constexpr int kRowHeight = 24;
constexpr int kInfoHeight = 20;
constexpr int kMaxFileRows = 250;

bool is_up(uint32_t k) { return k == LV_KEY_UP || k == ';'; }
bool is_down(uint32_t k) { return k == LV_KEY_DOWN || k == '.'; }
bool is_enter(uint32_t k) { return k == LV_KEY_ENTER || k == LV_KEY_RIGHT || k == '/'; }
bool is_back(uint32_t k) {
    return k == LV_KEY_LEFT || k == ',' || k == LV_KEY_BACKSPACE || k == LV_KEY_DEL;
}

std::string human(long long n) {
    char buf[32];
    double v = static_cast<double>(n);
    const char* units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%lld B", n);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    }
    return buf;
}

std::string lower_ext(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

const char* file_icon(const std::string& name) {
    static const char* images[] = {"jpg", "jpeg", "png", "gif", "bmp", "webp", "heic"};
    static const char* audio[] = {"mp3", "wav", "flac", "ogg", "m4a", "aac"};
    static const char* video[] = {"mp4", "mkv", "mov", "avi", "webm"};
    static const char* text[] = {"txt", "md", "pdf", "doc", "docx", "csv", "json", "log", "py",
                                 "sh", "conf"};
    const std::string ext = lower_ext(name);
    for (auto* e : images) if (ext == e) return kIconImage;
    for (auto* e : audio) if (ext == e) return kIconMusic;
    for (auto* e : video) if (ext == e) return kIconFilm;
    for (auto* e : text) if (ext == e) return kIconFileText;
    return kIconFile;
}

const char* device_icon(const std::string& type) {
    if (type == "mobile") return kIconMobile;
    if (type == "laptop") return kIconLaptop;
    if (type == "tablet") return kIconTablet;
    if (type == "headless" || type == "server") return kIconTerminal;
    if (type == "web") return kIconGlobe;
    return kIconDesktop;
}

const char* fallback_icon(const char* icon) {
    // Built-in LVGL symbols when the Phosphor font is unavailable.
    if (icon == kIconFolder) return LV_SYMBOL_DIRECTORY;
    if (icon == kIconUp) return LV_SYMBOL_UP;
    if (icon == kIconImage) return LV_SYMBOL_IMAGE;
    if (icon == kIconMusic) return LV_SYMBOL_AUDIO;
    if (icon == kIconFilm) return LV_SYMBOL_VIDEO;
    if (icon == kIconTray) return LV_SYMBOL_DOWNLOAD;
    if (icon == kIconChat) return LV_SYMBOL_ENVELOPE;
    if (icon == kIconMobile || icon == kIconTablet) return LV_SYMBOL_CALL;
    if (icon == kIconDesktop || icon == kIconLaptop || icon == kIconTerminal ||
        icon == kIconGlobe) {
        return LV_SYMBOL_HOME;
    }
    return LV_SYMBOL_FILE;
}

void set_hidden(lv_obj_t* obj, bool hidden) {
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// Keep a label on one line: LONG_DOT alone still wraps at spaces.
void one_line(lv_obj_t* label, const lv_font_t* font) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

// "/home/pi/very/long/path" -> ".../long/path" so the end stays visible.
std::string tail(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    std::string cut = text.substr(text.size() - (max_chars - 3));
    const auto slash = cut.find('/');
    if (slash != std::string::npos && slash + 1 < cut.size()) {
        cut = cut.substr(slash);
    }
    return "..." + cut;
}

lv_obj_t* plain_box(lv_obj_t* parent) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    return home && home[0] ? std::filesystem::path(home) : std::filesystem::path("/");
}

} // namespace

LsendScreen::LsendScreen(viewmodel::BaseViewModel& view_model,
                         app::AssetManager& assets,
                         lsend::Client& client)
    : BaseScreen(view_model, assets), client_(client) {
    font12_ = assets.load_standard_font(12, app::StandardFontWeight::Regular);
    if (!font12_) font12_ = &lv_font_montserrat_12;
    font14_ = assets.load_standard_font(14);
    if (!font14_) font14_ = &lv_font_montserrat_14;
    icon_font_ = assets.load_font("Phosphor-Fill.ttf", 16);
    cwd_ = home_dir();

    init();
    build_overlays();

    client_.set_event_handler([this](const json::Value& ev) { on_event(ev); });
    poll_timer_ = lv_timer_create(poll_timer_cb, 40, this);
    countdown_timer_ = lv_timer_create(countdown_timer_cb, 500, this);
    lv_timer_pause(countdown_timer_);

    set_mode(Mode::Home);
}

LsendScreen::~LsendScreen() {
    client_.set_event_handler(nullptr);
    if (poll_timer_) lv_timer_delete(poll_timer_);
    if (countdown_timer_) lv_timer_delete(countdown_timer_);
    toast_.reset();
    for (auto* obj : {incoming_box_, message_box_}) {
        if (obj && lv_obj_is_valid(obj)) lv_obj_delete(obj);
    }
}

// ------------------------------------------------------------------ build

void LsendScreen::build_content(lv_obj_t* content) {
    build_home(content);
    build_files(content);
    build_text(content);
    build_progress(content);
}

lv_obj_t* LsendScreen::make_row(lv_obj_t* parent, const char* icon, const std::string& name,
                                const std::string& right) {
    auto* row = plain_box(parent);
    lv_obj_set_size(row, LV_PCT(100), kRowHeight);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_left(row, 6, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);

    auto* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon_font_ ? icon : fallback_icon(icon));
    lv_obj_set_style_text_font(icon_label, icon_font_ ? icon_font_ : &lv_font_montserrat_14, 0);
    lv_obj_set_width(icon_label, 18);

    auto* name_label = lv_label_create(row);
    lv_label_set_text(name_label, name.c_str());
    lv_obj_set_style_text_font(name_label, font14_, 0);
    one_line(name_label, font14_);
    lv_obj_set_flex_grow(name_label, 1);

    auto* right_label = lv_label_create(row);
    lv_label_set_text(right_label, right.c_str());
    lv_obj_set_style_text_font(right_label, font12_, 0);
    lv_obj_set_style_text_opa(right_label, LV_OPA_70, 0);
    return row;
}

void LsendScreen::build_home(lv_obj_t* content) {
    const auto colors = view::palette(view_model().is_dark_mode());
    home_box_ = plain_box(content);
    lv_obj_set_size(home_box_, LV_PCT(100), kContentHeight);

    auto* info = plain_box(home_box_);
    lv_obj_set_size(info, LV_PCT(100), kInfoHeight);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 0);

    info_label_ = lv_label_create(info);
    lv_obj_set_width(info_label_, 200);
    lv_obj_set_style_text_font(info_label_, font12_, 0);
    one_line(info_label_, font12_);
    lv_obj_set_style_text_opa(info_label_, LV_OPA_80, 0);
    lv_obj_align(info_label_, LV_ALIGN_LEFT_MID, 8, 0);

    quick_label_ = lv_label_create(info);
    lv_label_set_recolor(quick_label_, true);
    lv_obj_set_style_text_font(quick_label_, font12_, 0);
    lv_obj_align(quick_label_, LV_ALIGN_RIGHT_MID, -8, 0);

    device_list_ = lv_obj_create(home_box_);
    lv_obj_remove_style_all(device_list_);
    lv_obj_set_size(device_list_, LV_PCT(100), kContentHeight - kInfoHeight);
    lv_obj_align(device_list_, LV_ALIGN_TOP_MID, 0, kInfoHeight);
    lv_obj_set_flex_flow(device_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(device_list_, 4, 0);
    lv_obj_set_style_pad_row(device_list_, 2, 0);
    lv_obj_set_scrollbar_mode(device_list_, LV_SCROLLBAR_MODE_OFF);

    empty_label_ = lv_label_create(home_box_);
    lv_label_set_long_mode(empty_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(empty_label_, 280);
    lv_obj_set_style_text_align(empty_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(empty_label_, font14_, 0);
    lv_obj_set_style_text_color(empty_label_, colors.text, 0);
    lv_obj_align(empty_label_, LV_ALIGN_CENTER, 0, kInfoHeight / 2);
}

void LsendScreen::build_files(lv_obj_t* content) {
    files_box_ = plain_box(content);
    lv_obj_set_size(files_box_, LV_PCT(100), kContentHeight);

    path_label_ = lv_label_create(files_box_);
    lv_obj_set_width(path_label_, view::kScreenWidth - 16);
    lv_obj_set_style_text_font(path_label_, font12_, 0);
    one_line(path_label_, font12_);
    lv_obj_set_style_text_opa(path_label_, LV_OPA_80, 0);
    lv_obj_align(path_label_, LV_ALIGN_TOP_LEFT, 8, 3);

    file_list_ = lv_obj_create(files_box_);
    lv_obj_remove_style_all(file_list_);
    lv_obj_set_size(file_list_, LV_PCT(100), kContentHeight - kInfoHeight);
    lv_obj_align(file_list_, LV_ALIGN_TOP_MID, 0, kInfoHeight);
    lv_obj_set_flex_flow(file_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(file_list_, 4, 0);
    lv_obj_set_style_pad_row(file_list_, 2, 0);
    lv_obj_set_scrollbar_mode(file_list_, LV_SCROLLBAR_MODE_OFF);
}

void LsendScreen::build_text(lv_obj_t* content) {
    text_box_ = plain_box(content);
    lv_obj_set_size(text_box_, LV_PCT(100), kContentHeight);

    text_title_ = lv_label_create(text_box_);
    lv_obj_set_width(text_title_, view::kScreenWidth - 16);
    lv_obj_set_style_text_font(text_title_, font12_, 0);
    one_line(text_title_, font12_);
    lv_obj_set_style_text_opa(text_title_, LV_OPA_80, 0);
    lv_obj_align(text_title_, LV_ALIGN_TOP_LEFT, 8, 3);

    textarea_ = lv_textarea_create(text_box_);
    lv_obj_set_size(textarea_, view::kScreenWidth - 12, kContentHeight - kInfoHeight - 6);
    lv_obj_align(textarea_, LV_ALIGN_TOP_MID, 0, kInfoHeight);
    lv_textarea_set_placeholder_text(textarea_, "Type a message...");
    lv_obj_set_style_text_font(textarea_, font14_, 0);
    lv_obj_add_state(textarea_, LV_STATE_FOCUSED);  // show the cursor
}

void LsendScreen::build_progress(lv_obj_t* content) {
    const auto colors = view::palette(view_model().is_dark_mode());
    progress_box_ = plain_box(content);
    lv_obj_set_size(progress_box_, view::kScreenWidth - 8, 30);
    lv_obj_align(progress_box_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(progress_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(progress_box_, colors.bar, 0);
    lv_obj_set_style_radius(progress_box_, 6, 0);
    lv_obj_set_style_border_width(progress_box_, 1, 0);
    lv_obj_set_style_border_color(progress_box_, colors.border, 0);

    progress_label_ = lv_label_create(progress_box_);
    lv_obj_set_width(progress_label_, 250);
    lv_obj_set_style_text_font(progress_label_, font12_, 0);
    one_line(progress_label_, font12_);
    lv_obj_set_style_text_color(progress_label_, colors.text, 0);
    lv_obj_align(progress_label_, LV_ALIGN_TOP_LEFT, 6, 2);

    progress_pct_ = lv_label_create(progress_box_);
    lv_obj_set_style_text_font(progress_pct_, font12_, 0);
    lv_obj_set_style_text_color(progress_pct_, colors.text, 0);
    lv_obj_align(progress_pct_, LV_ALIGN_TOP_RIGHT, -6, 2);

    progress_bar_ = lv_bar_create(progress_box_);
    lv_obj_set_size(progress_bar_, view::kScreenWidth - 22, 5);
    lv_obj_align(progress_bar_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(progress_bar_, 0, 1000);
    lv_obj_set_style_bg_color(progress_bar_, colors.border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar_, colors.primary, LV_PART_INDICATOR);

    set_hidden(progress_box_, true);
}

void LsendScreen::build_overlays() {
    const auto colors = view::palette(view_model().is_dark_mode());
    auto make_panel = [&](lv_obj_t*& box, lv_obj_t*& title) {
        box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, view::kScreenWidth - 20, view::kScreenHeight - 24);
        lv_obj_center(box);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(box, colors.surface, 0);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, colors.primary, 0);
        lv_obj_set_style_shadow_width(box, 12, 0);
        lv_obj_set_style_shadow_opa(box, LV_OPA_30, 0);
        lv_obj_set_style_pad_all(box, 10, 0);

        title = lv_label_create(box);
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_text_font(title, font14_, 0);
        one_line(title, font14_);
        lv_obj_set_style_text_color(title, colors.text, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
        set_hidden(box, true);
    };

    make_panel(incoming_box_, incoming_title_);
    incoming_files_ = lv_label_create(incoming_box_);
    lv_label_set_long_mode(incoming_files_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(incoming_files_, LV_PCT(100), 72);
    lv_obj_set_style_text_font(incoming_files_, font12_, 0);
    lv_obj_set_style_text_color(incoming_files_, colors.text, 0);
    lv_obj_align(incoming_files_, LV_ALIGN_TOP_LEFT, 0, 24);
    incoming_footer_ = lv_label_create(incoming_box_);
    lv_label_set_recolor(incoming_footer_, true);
    lv_obj_set_style_text_font(incoming_footer_, font12_, 0);
    lv_obj_set_style_text_color(incoming_footer_, colors.text, 0);
    lv_obj_align(incoming_footer_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    make_panel(message_box_, message_title_);
    message_text_ = lv_label_create(message_box_);
    lv_label_set_long_mode(message_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message_text_, LV_PCT(100));
    lv_obj_set_style_text_font(message_text_, font14_, 0);
    lv_obj_set_style_text_color(message_text_, colors.text, 0);
    lv_obj_align(message_text_, LV_ALIGN_TOP_LEFT, 0, 24);
    auto* close_hint = lv_label_create(message_box_);
    lv_obj_set_style_text_font(close_hint, font12_, 0);
    lv_obj_set_style_text_opa(close_hint, LV_OPA_60, 0);
    lv_obj_set_style_text_color(close_hint, colors.text, 0);
    lv_label_set_text(close_hint, "Any key to close");
    lv_obj_align(close_hint, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    toast_ = std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model(), assets());
    toast_->build();
}

// ------------------------------------------------------------------ modes

void LsendScreen::set_mode(Mode mode) {
    mode_ = mode;
    set_hidden(home_box_, mode != Mode::Home);
    set_hidden(files_box_, mode != Mode::Files);
    set_hidden(text_box_, mode != Mode::Text);
    if (mode == Mode::Home) {
        refresh_home();
    } else if (mode == Mode::Files) {
        load_dir(cwd_);
    } else {
        lv_textarea_set_text(textarea_, "");
        lv_label_set_text_fmt(text_title_, "Message to %s", target_.alias.c_str());
    }
    if (progress_box_) {
        lv_obj_move_foreground(progress_box_);
    }
    update_hints();
}

void LsendScreen::update_hints() {
    auto* bar = hint_bar();
    if (!bar) {
        return;
    }
    switch (mode_) {
        case Mode::Home:
            if (client_.devices().empty()) {
                bar->set_hints({{"R", "scan"}, {"S", "quick save"}, {"Esc", "hold to exit"}});
            } else {
                bar->set_hints({{"Enter", "file"}, {"T", "text"}, {"R", "scan"}, {"S", "quick save"}});
            }
            break;
        case Mode::Files:
            bar->set_hints({{"Enter", "send/open"}, {"Bksp", "up"}, {"A", "send folder"}, {"Esc", "back"}});
            break;
        case Mode::Text:
            bar->set_hints({{"Enter", "send"}, {"Esc", "cancel"}});
            break;
    }
}

void LsendScreen::select_row(std::vector<lv_obj_t*>& rows, int index) {
    const auto colors = view::palette(view_model().is_dark_mode());
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool sel = static_cast<int>(i) == index;
        lv_obj_set_style_bg_opa(rows[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(rows[i], colors.primary, 0);
        const lv_color_t fg = sel ? colors.background : colors.text;
        for (uint32_t c = 0; c < lv_obj_get_child_count(rows[i]); ++c) {
            lv_obj_set_style_text_color(lv_obj_get_child(rows[i], static_cast<int32_t>(c)), fg, 0);
        }
    }
    if (index >= 0 && index < static_cast<int>(rows.size())) {
        lv_obj_scroll_to_view(rows[static_cast<size_t>(index)], LV_ANIM_OFF);
    }
}

void LsendScreen::refresh_home() {
    if (!home_box_) {
        return;
    }
    const auto link = client_.link_state();
    if (link == lsend::LinkState::Connected) {
        lv_label_set_text_fmt(info_label_, "%s  |  %s", client_.alias().c_str(), client_.ip().c_str());
    } else {
        lv_label_set_text(info_label_, "LocalSend service offline");
    }
    const auto colors = view::palette(view_model().is_dark_mode());
    char quick[64];
    std::snprintf(quick, sizeof(quick), "Quick save #%06X %s#",
                  lv_color_to_u32(client_.auto_accept() ? colors.success : colors.text_disabled) & 0xFFFFFF,
                  client_.auto_accept() ? "ON" : "OFF");
    lv_label_set_text(quick_label_, quick);

    lv_obj_clean(device_list_);
    device_rows_.clear();
    const auto& devices = client_.devices();
    int sel = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        device_rows_.push_back(make_row(device_list_, device_icon(d.device_type), d.alias, d.ip));
        if (d.fingerprint == selected_fp_) {
            sel = static_cast<int>(i);
        }
    }
    device_sel_ = devices.empty() ? 0 : std::min(sel, static_cast<int>(devices.size()) - 1);
    if (!devices.empty()) {
        selected_fp_ = devices[static_cast<size_t>(device_sel_)].fingerprint;
    }
    select_row(device_rows_, device_sel_);

    const char* empty = nullptr;
    if (link == lsend::LinkState::Failed) {
        empty = "Can't start the lsend service.\nIs lsend installed? (sh install.sh)";
    } else if (link != lsend::LinkState::Connected) {
        empty = "Starting LocalSend service...";
    } else if (devices.empty()) {
        empty = "Looking for devices...\nOpen LocalSend on your phone";
    }
    set_hidden(empty_label_, empty == nullptr);
    if (empty) {
        lv_label_set_text(empty_label_, empty);
    }
    update_hints();
}

void LsendScreen::load_dir(const std::filesystem::path& dir, const std::string& select_name) {
    std::error_code ec;
    std::vector<Entry> list;
    for (auto it = std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!show_hidden_ && !name.empty() && name[0] == '.') {
            continue;
        }
        Entry e;
        e.name = name;
        std::error_code ec2;
        e.is_dir = it->is_directory(ec2);
        if (!e.is_dir) {
            e.size = it->file_size(ec2);
            if (ec2) e.size = 0;
        }
        list.push_back(std::move(e));
    }
    if (ec && list.empty() && dir != cwd_) {
        toast("Can't open folder", view::widgets::ToastTone::Error);
        return;
    }
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        std::string la = a.name, lb = b.name;
        for (auto& c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return la < lb;
    });
    cwd_ = dir;
    entries_ = std::move(list);
    file_sel_ = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == select_name) {
            file_sel_ = static_cast<int>(i);
        }
    }
    render_files();
}

void LsendScreen::render_files() {
    const std::string prefix = "To " + target_.alias + ":  ";
    const size_t room = prefix.size() < 40 ? 46 - prefix.size() : 8;
    lv_label_set_text_fmt(path_label_, "%s%s", prefix.c_str(), tail(cwd_.string(), room).c_str());
    lv_obj_clean(file_list_);
    file_rows_.clear();
    const size_t n = std::min(entries_.size(), static_cast<size_t>(kMaxFileRows));
    for (size_t i = 0; i < n; ++i) {
        const auto& e = entries_[i];
        file_rows_.push_back(make_row(file_list_, e.is_dir ? kIconFolder : file_icon(e.name), e.name,
                                      e.is_dir ? "" : human(static_cast<long long>(e.size))));
    }
    if (entries_.empty()) {
        auto* empty = lv_label_create(file_list_);
        lv_label_set_text(empty, "  (empty folder)");
        lv_obj_set_style_text_font(empty, font12_, 0);
        lv_obj_set_style_text_opa(empty, LV_OPA_60, 0);
    }
    file_sel_ = std::min(file_sel_, static_cast<int>(file_rows_.size()) - 1);
    select_row(file_rows_, std::max(file_sel_, 0));
}

// ------------------------------------------------------------------ events

void LsendScreen::on_event(const json::Value& ev) {
    const std::string type = ev["type"].str();
    if (type == "state" || type == "devices" || type == "connected" || type == "disconnected") {
        if (mode_ == Mode::Home) {
            refresh_home();
        }
        if (type == "disconnected") {
            hide_progress();
            hide_incoming();
        }
    } else if (type == "log") {
        toast(ev["msg"].str(), view::widgets::ToastTone::Default, 1800);
    } else if (type == "incoming") {
        show_incoming(ev);
    } else if (type == "incoming_done") {
        if (ev["id"].integer(-2) == incoming_id_) {
            hide_incoming();
        }
    } else if (type == "text") {
        show_message(ev["alias"].str("?"), ev["text"].str());
    } else if (type == "progress") {
        show_progress(ev["label"].str(), ev["done"].integer(), ev["total"].integer());
    } else if (type == "progress_end") {
        hide_progress();
        const bool ok = ev["ok"].boolean();
        toast(ev["msg"].str(), ok ? view::widgets::ToastTone::Success : view::widgets::ToastTone::Error,
              ok ? 3000 : 4000);
    }
}

void LsendScreen::show_progress(const std::string& label, long long done, long long total) {
    lv_label_set_text(progress_label_, label.c_str());
    if (total > 0) {
        const int permille = static_cast<int>(std::min<long long>(1000, done * 1000 / total));
        lv_bar_set_value(progress_bar_, permille, LV_ANIM_OFF);
        lv_label_set_text_fmt(progress_pct_, "%d%%", permille / 10);
    } else {
        lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        lv_label_set_text(progress_pct_, "");
    }
    set_hidden(progress_box_, false);
    lv_obj_move_foreground(progress_box_);
}

void LsendScreen::hide_progress() {
    set_hidden(progress_box_, true);
}

void LsendScreen::show_incoming(const json::Value& ev) {
    hide_message();
    incoming_id_ = static_cast<int>(ev["id"].integer(-1));
    const auto& files = ev["files"];
    long long total = 0;
    std::string names;
    for (size_t i = 0; i < files.size(); ++i) {
        total += files[i]["size"].integer();
        if (i < 4) {
            names += "  " + files[i]["name"].str("?") + "\n";
        }
    }
    if (files.size() > 4) {
        names += "  + " + std::to_string(files.size() - 4) + " more";
    }
    const std::string alias = ev["alias"].str("Someone");
    lv_label_set_text_fmt(incoming_title_, "%s wants to send %d file%s (%s)", alias.c_str(),
                          static_cast<int>(files.size()), files.size() == 1 ? "" : "s",
                          human(total).c_str());
    lv_label_set_text(incoming_files_, names.c_str());
    incoming_deadline_ = lv_tick_get() + static_cast<uint32_t>(ev["timeout"].integer(60)) * 1000U;
    update_incoming_countdown();
    set_hidden(incoming_box_, false);
    lv_obj_move_foreground(incoming_box_);
    lv_timer_resume(countdown_timer_);
}

void LsendScreen::update_incoming_countdown() {
    const auto colors = view::palette(view_model().is_dark_mode());
    const int32_t left_ms = static_cast<int32_t>(incoming_deadline_ - lv_tick_get());
    const int left = std::max(0, left_ms / 1000);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "#%06X Y# accept     #%06X N# decline          %ds",
                  lv_color_to_u32(colors.success) & 0xFFFFFF,
                  lv_color_to_u32(colors.error) & 0xFFFFFF, left);
    lv_label_set_text(incoming_footer_, buf);
    if (left_ms <= 0) {
        hide_incoming();
    }
}

void LsendScreen::hide_incoming() {
    incoming_id_ = -1;
    set_hidden(incoming_box_, true);
    if (countdown_timer_) {
        lv_timer_pause(countdown_timer_);
    }
}

void LsendScreen::show_message(const std::string& alias, const std::string& text) {
    lv_label_set_text_fmt(message_title_, "Message from %s", alias.c_str());
    lv_label_set_text(message_text_, text.c_str());
    set_hidden(message_box_, false);
    lv_obj_move_foreground(message_box_);
}

void LsendScreen::hide_message() {
    set_hidden(message_box_, true);
}

void LsendScreen::toast(const std::string& msg, view::widgets::ToastTone tone, uint32_t ms) {
    if (toast_ && !msg.empty()) {
        toast_->show(msg, tone, ms);
    }
}

// ------------------------------------------------------------------ keys

bool LsendScreen::handle_key(uint32_t key, bool long_pressed) {
    // Pop-ups take priority.
    if (incoming_id_ >= 0 && !lv_obj_has_flag(incoming_box_, LV_OBJ_FLAG_HIDDEN)) {
        if (long_pressed) {
            return true;
        }
        if (key == 'y' || key == 'Y' || key == LV_KEY_ENTER) {
            client_.answer(incoming_id_, true);
            toast("Accepted", view::widgets::ToastTone::Success, 1500);
            hide_incoming();
        } else if (key == 'n' || key == 'N' || key == LV_KEY_ESC) {
            client_.answer(incoming_id_, false);
            hide_incoming();
        }
        return true;
    }
    if (!lv_obj_has_flag(message_box_, LV_OBJ_FLAG_HIDDEN)) {
        if (!long_pressed) {
            hide_message();
        }
        return true;
    }

    if (long_pressed) {
        if (mode_ == Mode::Home && key == LV_KEY_ESC && exit_armed_) {
            toast_->hide();
            view_model().request_quit();
        }
        return true;
    }

    switch (mode_) {
        case Mode::Home:
            return home_key(key, long_pressed);
        case Mode::Files:
            return files_key(key);
        case Mode::Text:
            return text_key(key);
    }
    return false;
}

void LsendScreen::handle_release(uint32_t key) {
    if (key == LV_KEY_ESC && exit_armed_) {
        exit_armed_ = false;
        toast_->hide();
    }
}

bool LsendScreen::home_key(uint32_t key, bool) {
    const auto& devices = client_.devices();
    const int count = static_cast<int>(devices.size());
    if (key == LV_KEY_ESC) {
        exit_armed_ = true;
        toast_->show_persistent_highlighted("Hold ", "ESC", " to exit", view::widgets::ToastTone::Warning);
    } else if (key == 'q' || key == 'Q') {
        view_model().request_quit();
    } else if (is_up(key) && count) {
        device_sel_ = (device_sel_ + count - 1) % count;
        selected_fp_ = devices[static_cast<size_t>(device_sel_)].fingerprint;
        select_row(device_rows_, device_sel_);
    } else if (is_down(key) && count) {
        device_sel_ = (device_sel_ + 1) % count;
        selected_fp_ = devices[static_cast<size_t>(device_sel_)].fingerprint;
        select_row(device_rows_, device_sel_);
    } else if (key == 'r' || key == 'R') {
        client_.scan();
    } else if (key == 's' || key == 'S') {
        client_.set_auto_accept(!client_.auto_accept());
        toast(client_.auto_accept() ? "Quick save off: you'll be asked"
                                    : "Quick save on: files save automatically",
              view::widgets::ToastTone::Default, 2200);
    } else if ((is_enter(key) || key == 't' || key == 'T') && count) {
        target_ = devices[static_cast<size_t>(device_sel_)];
        set_mode((key == 't' || key == 'T') ? Mode::Text : Mode::Files);
    } else {
        return false;
    }
    return true;
}

void LsendScreen::start_send_paths(const std::vector<std::string>& paths) {
    client_.send_files(target_.fingerprint, paths);
    set_mode(Mode::Home);
    show_progress("Waiting for " + target_.alias + " to accept...", 0, 0);
}

bool LsendScreen::files_key(uint32_t key) {
    const int count = static_cast<int>(file_rows_.size());
    if (key == LV_KEY_ESC) {
        set_mode(Mode::Home);
    } else if (is_up(key) && count) {
        file_sel_ = (file_sel_ + count - 1) % count;
        select_row(file_rows_, file_sel_);
    } else if (is_down(key) && count) {
        file_sel_ = (file_sel_ + 1) % count;
        select_row(file_rows_, file_sel_);
    } else if (is_back(key)) {
        if (cwd_.has_parent_path() && cwd_ != cwd_.root_path()) {
            const std::string child = cwd_.filename().string();
            load_dir(cwd_.parent_path(), child);
        }
    } else if (is_enter(key) && count) {
        const auto& e = entries_[static_cast<size_t>(file_sel_)];
        const auto path = cwd_ / e.name;
        if (e.is_dir) {
            load_dir(path);
        } else {
            start_send_paths({path.string()});
        }
    } else if (key == 'a' || key == 'A') {
        start_send_paths({cwd_.string()});
    } else if (key == 'h' || key == 'H') {
        show_hidden_ = !show_hidden_;
        load_dir(cwd_);
    } else {
        return false;
    }
    return true;
}

bool LsendScreen::text_key(uint32_t key) {
    if (key == LV_KEY_ESC) {
        set_mode(Mode::Home);
    } else if (key == LV_KEY_ENTER) {
        const std::string text = lv_textarea_get_text(textarea_);
        if (!text.empty()) {
            client_.send_text(target_.fingerprint, text);
            set_mode(Mode::Home);
            show_progress("Sending message to " + target_.alias + "...", 0, 0);
        }
    } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        lv_textarea_delete_char(textarea_);
    } else if (key == LV_KEY_LEFT) {
        lv_textarea_cursor_left(textarea_);
    } else if (key == LV_KEY_RIGHT) {
        lv_textarea_cursor_right(textarea_);
    } else if (key >= 32 && key < 127) {
        lv_textarea_add_char(textarea_, key);
    } else {
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ timers

void LsendScreen::poll_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<LsendScreen*>(lv_timer_get_user_data(timer));
    const auto before = self->client_.link_state();
    self->client_.poll();
    if (self->client_.link_state() != before && self->mode_ == Mode::Home) {
        self->refresh_home();
    }
}

void LsendScreen::countdown_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<LsendScreen*>(lv_timer_get_user_data(timer));
    if (self->incoming_id_ >= 0) {
        self->update_incoming_countdown();
    }
}

} // namespace screen
