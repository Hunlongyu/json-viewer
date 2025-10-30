#pragma once

#include <SimpleWindow.h>
#include <simdjson.h>

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

    std::wstring               m_file;   // 文件名
    simdjson::ondemand::parser m_parser; // 解析器
    simdjson::padded_string    m_json;   // JSON字符串

    void init_ui();

    void drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e);

    void parse_file();
    void fill_tree_root();
    void process_value(sw::TreeViewNode &parent, simdjson::ondemand::value value);

    void on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e);
};