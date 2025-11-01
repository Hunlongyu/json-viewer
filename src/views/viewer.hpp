#pragma once

#include <SimpleWindow.h>
#include <filesystem>
#include <simdjson.h>

using namespace simdjson;
using namespace simdjson::ondemand;
namespace fs = std::filesystem;

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

    // UI初始化
    void init_ui();

    // 拖拽文件
    void drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e);

    // 解析文件
    void parse_file();

    // 填充根节点
    void fill_tree_root();

    // 填充树
    void fill_tree_with_path(
        object object, sw::TreeView &tree_view, const std::string &base_path, const document &doc
    );
    void fill_tree_with_path(
        array array, sw::TreeView &tree_view, const std::string &base_path, document &doc
    );
    void fill_tree_with_path(
        object                  object,
        const sw::TreeViewNode &parent,
        const std::string      &base_path,
        const document         &doc
    );
    static void fill_tree_with_path(
        array array, const sw::TreeViewNode &parent, const std::string &base_path, document &doc
    );
    // 懒加载填充树
    void fill_tree_lazy(const sw::TreeViewNode &node);

    // 获取值预览
    static std::wstring get_value_preview(value value);

    // 节点是否有子节点
    static bool has_children(value value);

    // 添加占位符子节点
    static void add_placeholder_child(const sw::TreeViewNode &parent, const std::string &json_path);

    // 树节点展开事件
    void on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e);

    // 获取文件名
    static std::wstring get_filename_from_path(std::wstring_view path);

    // 构建对象路径
    [[nodiscard]] static std::string build_object_path(
        std::string_view base_path, std::string_view key
    );

    // 构建数组路径
    [[nodiscard]] static std::string build_array_path(std::string_view base_path, int index);
};