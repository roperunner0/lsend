/*
 * LocalSend main screen: device list, file picker, text entry,
 * progress, incoming-transfer prompt and message viewer.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"
#include "lsend_client.h"
#include "toast.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace screen {

class LsendScreen : public BaseScreen {
public:
    LsendScreen(viewmodel::BaseViewModel& view_model,
                app::AssetManager& assets,
                lsend::Client& client);
    ~LsendScreen() override;

    // Returns true if the key was consumed.
    bool handle_key(uint32_t key, bool long_pressed);
    void handle_release(uint32_t key);

private:
    enum class Mode { Home, Files, Text };

    struct Entry {
        std::string name;
        bool is_dir{false};
        uintmax_t size{0};
    };

    void build_content(lv_obj_t* content) override;
    void build_home(lv_obj_t* content);
    void build_files(lv_obj_t* content);
    void build_text(lv_obj_t* content);
    void build_progress(lv_obj_t* content);
    void build_overlays();

    void set_mode(Mode mode);
    void update_hints();
    void refresh_home();
    void load_dir(const std::filesystem::path& dir, const std::string& select_name = {});
    void render_files();
    void select_row(std::vector<lv_obj_t*>& rows, int index);
    lv_obj_t* make_row(lv_obj_t* parent, const char* icon, const std::string& name,
                       const std::string& right);

    void on_event(const json::Value& ev);
    void show_progress(const std::string& label, long long done, long long total);
    void hide_progress();
    void show_incoming(const json::Value& ev);
    void hide_incoming();
    void update_incoming_countdown();
    void show_message(const std::string& alias, const std::string& text);
    void hide_message();
    void toast(const std::string& msg, view::widgets::ToastTone tone, uint32_t ms = 2600);

    bool home_key(uint32_t key, bool long_pressed);
    bool files_key(uint32_t key);
    bool text_key(uint32_t key);
    void start_send_paths(const std::vector<std::string>& paths);

    static void poll_timer_cb(lv_timer_t* timer);
    static void countdown_timer_cb(lv_timer_t* timer);

    lsend::Client& client_;
    Mode mode_{Mode::Home};

    const lv_font_t* font12_{nullptr};
    const lv_font_t* font14_{nullptr};
    const lv_font_t* icon_font_{nullptr};

    // home
    lv_obj_t* home_box_{nullptr};
    lv_obj_t* info_label_{nullptr};
    lv_obj_t* quick_label_{nullptr};
    lv_obj_t* device_list_{nullptr};
    lv_obj_t* empty_label_{nullptr};
    std::vector<lv_obj_t*> device_rows_;
    int device_sel_{0};
    std::string selected_fp_;

    // files
    lv_obj_t* files_box_{nullptr};
    lv_obj_t* path_label_{nullptr};
    lv_obj_t* file_list_{nullptr};
    std::vector<lv_obj_t*> file_rows_;
    std::vector<Entry> entries_;
    std::filesystem::path cwd_;
    int file_sel_{0};
    bool show_hidden_{false};
    lsend::Device target_;

    // text
    lv_obj_t* text_box_{nullptr};
    lv_obj_t* text_title_{nullptr};
    lv_obj_t* textarea_{nullptr};

    // progress strip
    lv_obj_t* progress_box_{nullptr};
    lv_obj_t* progress_label_{nullptr};
    lv_obj_t* progress_pct_{nullptr};
    lv_obj_t* progress_bar_{nullptr};

    // overlays
    lv_obj_t* incoming_box_{nullptr};
    lv_obj_t* incoming_title_{nullptr};
    lv_obj_t* incoming_files_{nullptr};
    lv_obj_t* incoming_footer_{nullptr};
    int incoming_id_{-1};
    uint32_t incoming_deadline_{0};
    lv_obj_t* message_box_{nullptr};
    lv_obj_t* message_title_{nullptr};
    lv_obj_t* message_text_{nullptr};

    std::unique_ptr<view::widgets::Toast> toast_;
    lv_timer_t* poll_timer_{nullptr};
    lv_timer_t* countdown_timer_{nullptr};
    bool exit_armed_{false};
};

} // namespace screen
