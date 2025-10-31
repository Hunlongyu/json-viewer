#pragma once

#include <SimpleWindow.h>
#include <simdjson.h>

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

    std::map<sw::TreeViewNode *, std::string> m_node_paths; // 存储节点路径映射，用于懒加载

    void init_ui();

    void drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e);

    void parse_file();
    void fill_tree_root();
    void fill_tree(object object, sw::TreeView &tree_view);
    void fill_tree(array array, sw::TreeView &tree_view);
    void fill_tree(object object, sw::TreeViewNode &parent);
    void fill_tree(array array, sw::TreeViewNode &parent);
    void fill_tree(value value, sw::TreeViewNode &parent);

    void fill_tree_with_path(object object, sw::TreeView &tree_view, const std::string &base_path);
    void fill_tree_with_path(array array, sw::TreeView &tree_view, const std::string &base_path);
    void fill_tree_with_path(object object, sw::TreeViewNode &parent, const std::string &base_path);
    void fill_tree_with_path(array array, sw::TreeViewNode &parent, const std::string &base_path);

    void fill_tree_lazy(sw::TreeViewNode &node);
    void fill_tree_lazy_with_path(sw::TreeViewNode &node, const std::string &path);

    static std::wstring get_value_preview(value value);
    bool                has_children(value value);
    void                add_placeholder_child(sw::TreeViewNode &parent);

    void on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e);
};