#pragma once

#include <SimpleWindow.h>
#include <chrono>
#include <simdjson.h>

#include <string>

using namespace simdjson;
using namespace simdjson::ondemand;

class Viewer : public sw::Window
{
  public:
    Viewer();
    ~Viewer() override = default;

  private:
    sw::Grid     m_grid;     // 布局网格
    sw::TreeView m_treeView; // 控件：树

    std::atomic_bool m_dropping = false; // 拖拽文件
    std::atomic_bool m_parsing  = false; // 解析文件

    std::wstring  m_file;   // 文件名
    parser        m_parser; // 解析器
    padded_string m_json;   // JSON字符串

    // 性能计时相关
    std::chrono::high_resolution_clock::time_point m_start_time;
    std::chrono::high_resolution_clock::time_point m_load_time;
    std::chrono::high_resolution_clock::time_point m_parse_time;

    void init_ui();

    void drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e);

    void parse_file();
    void fill_tree_root();

    void fill_tree_with_path(object object, sw::TreeView &tree_view, const std::string &base_path);
    void fill_tree_with_path(array array, sw::TreeView &tree_view, const std::string &base_path);
    void fill_tree_with_path(
        object object, const sw::TreeViewNode &parent, const std::string &base_path
    );
    void fill_tree_with_path(
        array array, const sw::TreeViewNode &parent, const std::string &base_path
    );

    void fill_tree_lazy(sw::TreeViewNode &node);

    static std::wstring get_value_preview(value value);
    bool                has_children(value value);
    static void add_placeholder_child(const sw::TreeViewNode &parent, const std::string &json_path);

    void on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e);

    // 性能计时相关方法
    void start_timing();
    void log_load_time();
    void log_parse_time();
    void log_display_time();
};