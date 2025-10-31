#include "viewer.hpp"

Viewer::Viewer()
{
    init_ui();
}

void Viewer::init_ui()
{
    Text            = L"JSON Viewer";
    StartupLocation = sw::WindowStartupLocation::CenterScreen;
    Width           = 580;
    Height          = 800;
    SetLayout<sw::FillLayout>();
    HorizontalAlignment = sw::HorizontalAlignment::Stretch;
    VerticalAlignment   = sw::VerticalAlignment::Stretch;

    m_grid.HorizontalAlignment = sw::HorizontalAlignment::Stretch;
    m_grid.VerticalAlignment   = sw::VerticalAlignment::Stretch;
    m_grid.SetRows({sw::FillRemainGridRow{}});
    m_grid.SetColumns({sw::FillRemainGridColumn{}});
    AddChild(m_grid);

    m_treeView.HorizontalAlignment = sw::HorizontalAlignment::Stretch;
    m_treeView.VerticalAlignment   = sw::VerticalAlignment::Stretch;
    m_treeView.SetStyle(WS_BORDER, false);
    m_grid.AddChild(m_treeView, sw::GridLayoutTag{0, 0});

    DragAcceptFiles(Handle, true);
    RegisterRoutedEvent<sw::DropFilesEventArgs>(*this, &Viewer::drop_file);

    RegisterRoutedEvent<sw::TreeViewItemExpandedEventArgs>(
        *this, &Viewer::on_tree_view_item_expanded
    );
}

void Viewer::drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e)
{
    if (m_dropping)
    {
        return;
    }
    m_dropping = true;

    const auto count = DragQueryFileW(e.hDrop, -1, nullptr, 0);
    if (count == 0)
    {
        DragFinish(e.hDrop);
        m_dropping = false;
        return;
    }

    if (count >= 1)
    {
        wchar_t filePath[MAX_PATH];
        DragQueryFileW(e.hDrop, 0, filePath, MAX_PATH);
        m_file = filePath;
    }

    DragFinish(e.hDrop);
    m_dropping = false;

    if (!m_file.empty())
    {
        parse_file();
    }
}

void Viewer::parse_file()
{
    if (m_parsing)
    {
        return;
    }
    m_parsing = true;
    m_treeView.Clear();
    m_node_paths.clear();
    try
    {
        const auto file = sw::Utils::ToMultiByteStr(m_file);
        m_json          = padded_string::load(file);
        fill_tree_root();
    }
    catch (...)
    {
    }
    m_parsing = false;
}

void Viewer::fill_tree_root()
{
    try
    {
        document doc        = m_parser.iterate(m_json);
        auto     root_value = doc.get_value();

        auto root_result = root_value.type();
        if (root_result.error() != SUCCESS)
        {
            return;
        }

        auto root_type = root_result.value();
        if (root_type == json_type::object)
        {
            fill_tree_with_path(root_value.get_object(), m_treeView, "");
        }
        if (root_type == json_type::array)
        {
            fill_tree_with_path(root_value.get_array(), m_treeView, "");
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree(object object, sw::TreeView &tree_view)
{
    try
    {
        for (auto field : object)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view));

            auto value_result = field.value();
            if (value_result.error() != SUCCESS)
            {
                continue;
            }
            auto value             = value_result.value();
            auto value_type_result = value.type();
            if (value_type_result.error() != SUCCESS)
            {
                continue;
            }

            std::wstring value_preview = get_value_preview(value);
            auto         text          = key + L": " + value_preview;
            auto         node          = tree_view.AddItem(text);
            if (has_children(value))
            {
                add_placeholder_child(node);
            }
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree(array array, sw::TreeView &tree_view)
{
    try
    {
        int idx = 1;
        for (auto element : array)
        {
            const auto value = get_value_preview(element.value());
            const auto text  = std::to_wstring(idx) + L": " + value;
            auto       node  = tree_view.AddItem(text);
            if (has_children(element.value()))
            {
                add_placeholder_child(node);
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree(object object, sw::TreeViewNode &parent)
{
    try
    {
        for (auto field : object)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view));

            auto value_result = field.value();
            if (value_result.error() != SUCCESS)
                continue;
            auto value             = value_result.value();
            auto value_type_result = value.type();
            if (value_type_result.error() != SUCCESS)
                continue;

            std::wstring value_preview = get_value_preview(value);
            auto         text          = key + L": " + value_preview;
            auto         node          = parent.AddChild(text);

            // 如果值有子项，添加占位符以显示展开按钮
            if (has_children(value))
            {
                add_placeholder_child(node);
            }
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree(array array, sw::TreeViewNode &parent)
{
    try
    {
        int idx = 1;
        for (auto element : array)
        {
            const auto value = get_value_preview(element.value());
            const auto text  = std::to_wstring(idx) + L": " + value;
            auto       node  = parent.AddChild(text);

            // 如果元素有子项，添加占位符以显示展开按钮
            if (has_children(element.value()))
            {
                add_placeholder_child(node);
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree(value value, sw::TreeViewNode &parent)
{
    try
    {
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
            return;

        auto type = value_type_result.value();
        if (type == ondemand::json_type::object)
        {
            fill_tree(value.get_object(), parent);
        }
        else if (type == ondemand::json_type::array)
        {
            fill_tree(value.get_array(), parent);
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(
    object object, sw::TreeView &tree_view, const std::string &base_path
)
{
    try
    {
        for (auto field : object)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view));
            std::string      key_str  = std::string(key_view);

            auto value_result = field.value();
            if (value_result.error() != SUCCESS)
                continue;
            auto value             = value_result.value();
            auto value_type_result = value.type();
            if (value_type_result.error() != SUCCESS)
                continue;

            std::wstring value_preview = get_value_preview(value);
            auto         text          = key + L": " + value_preview;
            auto         node          = tree_view.AddItem(text);

            // 构建当前项的路径
            std::string current_path = base_path.empty() ? key_str : base_path + "." + key_str;

            // 如果值有子项，添加占位符并存储路径
            if (has_children(value))
            {
                add_placeholder_child(node);
                m_node_paths[&node] = current_path;
            }
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(array array, sw::TreeView &tree_view, const std::string &base_path)
{
    try
    {
        int idx = 0;
        for (auto element : array)
        {
            const auto value = get_value_preview(element.value());
            const auto text  = std::to_wstring(idx) + L": " + value;
            auto       node  = tree_view.AddItem(text);

            // 构建当前项的路径
            std::string current_path = base_path + "[" + std::to_string(idx) + "]";

            // 如果元素有子项，添加占位符并存储路径
            if (has_children(element.value()))
            {
                add_placeholder_child(node);
                m_node_paths[&node] = current_path;
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(
    object object, sw::TreeViewNode &parent, const std::string &base_path
)
{
    try
    {
        for (auto field : object)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view));
            std::string      key_str  = std::string(key_view);

            auto value_result = field.value();
            if (value_result.error() != SUCCESS)
                continue;
            auto value             = value_result.value();
            auto value_type_result = value.type();
            if (value_type_result.error() != SUCCESS)
                continue;

            std::wstring value_preview = get_value_preview(value);
            auto         text          = key + L": " + value_preview;
            auto         node          = parent.AddChild(text);

            // 构建当前项的路径
            std::string current_path = base_path.empty() ? key_str : base_path + "." + key_str;

            // 如果值有子项，添加占位符并存储路径
            if (has_children(value))
            {
                add_placeholder_child(node);
                m_node_paths[&node] = current_path;
            }
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(
    array array, sw::TreeViewNode &parent, const std::string &base_path
)
{
    try
    {
        int idx = 0;
        for (auto element : array)
        {
            const auto value = get_value_preview(element.value());
            const auto text  = std::to_wstring(idx) + L": " + value;
            auto       node  = parent.AddChild(text);

            // 构建当前项的路径
            std::string current_path = base_path + "[" + std::to_string(idx) + "]";

            // 如果元素有子项，添加占位符并存储路径
            if (has_children(element.value()))
            {
                add_placeholder_child(node);
                m_node_paths[&node] = current_path;
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_lazy(sw::TreeViewNode &node)
{ // 查找节点对应的路径
    auto it = m_node_paths.find(&node);
    if (it != m_node_paths.end())
    {
        fill_tree_lazy_with_path(node, it->second);
    }
    else
    {
        // 如果找不到路径，添加错误提示
        node.AddChild(L"[路径信息丢失]");
    }
}

void Viewer::fill_tree_lazy_with_path(sw::TreeViewNode &node, const std::string &path)
{
    try
    {
        // 重新解析JSON并导航到指定路径
        ondemand::document doc        = m_parser.iterate(m_json);
        auto               root_value = doc.get_value();

        // 解析路径并导航到对应的值
        auto current_value = root_value;

        if (!path.empty())
        {
            // 简单的路径解析实现
            // 路径格式: "key1.key2[0].key3" 等
            // 这里实现一个基本版本，可以根据需要扩展

            // 暂时实现一个简化版本
            // 实际应用中需要更复杂的路径解析逻辑
        }

        // 根据值的类型填充子项
        auto value_type_result = current_value.type();
        if (value_type_result.error() == SUCCESS)
        {
            auto type = value_type_result.value();
            if (type == ondemand::json_type::object)
            {
                fill_tree_with_path(current_value.get_object(), node, path);
            }
            else if (type == ondemand::json_type::array)
            {
                fill_tree_with_path(current_value.get_array(), node, path);
            }
        }
    }
    catch (...)
    {
        node.AddChild(L"[加载失败]");
    }
}

std::wstring Viewer::get_value_preview(value value)
{
    try
    {
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
        {
            return L"<error>";
        }
        switch (value_type_result.value())
        {
        case json_type::unknown: {
            return L"<unknown>";
        }
        case json_type::string: {
            const std::string_view value_view = value.get_string();
            const std::wstring     value_str  = sw::Utils::ToWideStr(std::string(value_view));
            return L"\"" + value_str + L"\"";
        }
        case json_type::number: {
            const auto token        = value.raw_json_token();
            const auto string_token = sw::Utils::ToWideStr(std::string(token));
            return string_token;
        }
        case json_type::boolean: {
            const bool val = value.get_bool();
            return val ? L"true" : L"false";
        }
        case json_type::null: {
            return L"null";
        }
        case json_type::object: {
            auto       obj   = value.get_object();
            const auto count = obj.count_fields();
            if (count.error() == SUCCESS)
            {
                return L"{" + std::to_wstring(count.value_unsafe()) + L"}";
            }
            return L"{...}";
        }
        case json_type::array: {
            auto       obj   = value.get_array();
            const auto count = obj.count_elements();
            if (count.error() == SUCCESS)
            {
                return L"[" + std::to_wstring(count.value_unsafe()) + L"]";
            }
            return L"[...]";
        }
        }
    }
    catch (...)
    {
        return L"<error>";
    }
    return L"<error>";
}

bool Viewer::has_children(value value)
{
    try
    {
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
        {
            return false;
        }

        auto type = value_type_result.value();
        return (type == ondemand::json_type::object || type == ondemand::json_type::array);
    }
    catch (...)
    {
        return false;
    }
}

void Viewer::add_placeholder_child(sw::TreeViewNode &parent)
{
    parent.AddChild(L"Loading...");
}

void Viewer::on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e)
{
    auto      &node       = e.node;
    const auto isExpanded = node.IsExpanded();
    const auto isChecked  = node.IsChecked();
    const auto isSelected = node.IsSelected();

    //// 检查是否已经加载过子项（如果只有一个"Loading..."子项，说明还没加载）
    // if (node.IsExpanded() == 1)
    //{
    //     auto firstChild = node.GetChildAt(0);
    //     if (firstChild.GetText() == L"Loading...")
    //     {
    //         // 移除占位符
    //         node.RemoveChild(firstChild);

    //        // 使用路径信息进行懒加载
    //        fill_tree_lazy(node);
    //    }
    //}
}