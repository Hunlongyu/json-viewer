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
    if (m_dropping.exchange(true))
    {
        return;
    }

    // 重置窗口标题
    Text = L"JSON Viewer - 处理中...";

    const auto count = DragQueryFileW(e.hDrop, -1, nullptr, 0);
    if (count == 0)
    {
        DragFinish(e.hDrop);
        m_dropping = false;
        Text       = L"JSON Viewer";
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
    if (m_parsing.exchange(true))
    {
        return;
    }

    // 开始计时
    start_timing();

    try
    {
        // 记录加载开始时间
        m_load_time = std::chrono::high_resolution_clock::now();

        m_json = padded_string::load(sw::Utils::ToMultiByteStr(m_file));

        // 记录解析开始时间
        m_parse_time = std::chrono::high_resolution_clock::now();

        fill_tree_root();

        // 记录性能日志
        log_load_time();
        log_parse_time();
        log_display_time();
    }
    catch (...)
    {
        m_treeView.Clear();
        m_treeView.AddItem(L"[文件加载或解析失败]");
        Text = L"JSON Viewer - 解析失败";
        OutputDebugStringW(L"JSON文件解析失败\n");
    }

    m_parsing = false;
}

void Viewer::fill_tree_root()
{
    m_treeView.Clear();
    try
    {
        ondemand::document doc               = m_parser.iterate(m_json);
        auto               root_value        = doc.get_value();
        auto               value_type_result = root_value.type();

        if (value_type_result.error() == SUCCESS)
        {
            const auto type = value_type_result.value();
            if (type == json_type::object)
            {
                fill_tree_with_path(root_value.get_object(), m_treeView, "$");
            }
            else if (type == json_type::array)
            {
                fill_tree_with_path(root_value.get_array(), m_treeView, "$");
            }
            else
            {
                m_treeView.AddItem(L"根节点不是对象或数组");
            }
        }
    }
    catch (...)
    {
        m_treeView.AddItem(L"[解析失败]");
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
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view), true);

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
                std::string current_path;
                if (base_path.empty() || base_path == "$")
                {
                    current_path = "$." + std::string(key_view);
                }
                else
                {
                    current_path = base_path + "." + std::string(key_view);
                }
                add_placeholder_child(node, current_path);
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
            auto value_type_result = element.type();
            if (value_type_result.error() != SUCCESS)
            {
                ++idx;
                continue;
            }
            const auto value         = element.value();
            auto       value_preview = get_value_preview(value);

            const auto text = std::to_wstring(idx) + L": " + value_preview;
            auto       node = tree_view.AddItem(text);

            if (has_children(value))
            {
                std::string current_path;
                if (base_path.empty() || base_path == "$")
                {
                    current_path = "$[" + std::to_string(idx) + "]";
                }
                else
                {
                    current_path = base_path + "[" + std::to_string(idx) + "]";
                }
                add_placeholder_child(node, current_path);
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(
    object object, const sw::TreeViewNode &parent, const std::string &base_path
)
{
    try
    {
        for (auto field : object)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view), true);

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

            if (has_children(value))
            {
                std::string current_path;
                if (base_path.empty() || base_path == "$")
                {
                    current_path = "$." + std::string(key_view);
                }
                else
                {
                    current_path = base_path + "." + std::string(key_view);
                }
                add_placeholder_child(node, current_path);
            }
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_with_path(
    array array, const sw::TreeViewNode &parent, const std::string &base_path
)
{
    try
    {
        int idx = 0;
        for (auto element : array)
        {
            const auto value         = element.value();
            auto       value_preview = get_value_preview(value);

            const auto text = std::to_wstring(idx) + L": " + value_preview;
            auto       node = parent.AddChild(text);

            if (has_children(value))
            {
                // 构建当前项的路径 (JSONPath 格式)
                std::string current_path;
                if (base_path.empty() || base_path == "$")
                {
                    current_path = "$[" + std::to_string(idx) + "]";
                }
                else
                {
                    current_path = base_path + "[" + std::to_string(idx) + "]";
                }
                add_placeholder_child(node, current_path);
            }
            ++idx;
        }
    }
    catch (...)
    {
    }
}

void Viewer::fill_tree_lazy(sw::TreeViewNode &node)
{
    try
    {
        const std::string *json_path_ptr = static_cast<std::string *>(node.GetUserData());
        if (json_path_ptr == nullptr)
        {
            node.AddChild(L"[路径信息丢失]");
            return;
        }
        const std::string json_path = *json_path_ptr;
        if (json_path.empty())
        {
            node.AddChild(L"[路径信息丢失]");
            return;
        }

        // 重新解析 JSON 文档
        ondemand::document doc = m_parser.iterate(m_json);

        // 使用 simdjson 的 at_path 方法导航到指定路径
        auto value_result = doc.at_path(json_path);
        if (value_result.error() != SUCCESS)
        {
            node.AddChild(L"[路径无效: ]");
            return;
        }

        auto value             = value_result.value();
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
        {
            node.AddChild(L"[无法获取类型]");
            return;
        }

        const auto type = value_type_result.value();
        if (type == json_type::object)
        {
            fill_tree_with_path(value.get_object(), node, json_path);
        }
        else if (type == json_type::array)
        {
            fill_tree_with_path(value.get_array(), node, json_path);
        }
        else
        {
            node.AddChild(L"[不是对象或数组]");
        }
    }
    catch (...)
    {
        node.AddChild(L"[懒加载失败]");
    }
}

std::wstring Viewer::get_value_preview(value value)
{
    try
    {
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
        {
            return L"[类型错误]";
        }

        const auto type = value_type_result.value();
        switch (type)
        {
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
        case json_type::string: {
            const std::string_view str_view = value.get_string();
            const std::wstring     str      = sw::Utils::ToWideStr(std::string(str_view), true);
            return L"\"" + str + L"\"";
        }
        case json_type::number: {
            const auto token        = value.raw_json_token();
            const auto string_token = sw::Utils::ToWideStr(std::string(token), true);
            return string_token;
        }
        case json_type::boolean: {
            const bool val = value.get_bool();
            return val ? L"true" : L"false";
        }
        case json_type::null:
            return L"null";
        default:
            return L"[未知类型]";
        }
    }
    catch (...)
    {
        return L"[预览失败]";
    }
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

        const auto type = value_type_result.value();
        return (type == json_type::object || type == json_type::array);
    }
    catch (...)
    {
        return false;
    }
}

void Viewer::add_placeholder_child(const sw::TreeViewNode &parent, const std::string &json_path)
{
    parent.AddChild(L"Loading...");
    std::string *json_path_ptr = new std::string(json_path);
    parent.SetUserData(json_path_ptr);
}

void Viewer::on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e)
{
    if (e.action)
    {
        const sw::TreeViewNode firstChild = e.node.GetFirstChildNode();
        if (!firstChild.IsNull() && firstChild.GetText() == L"Loading...")
        {
            if (firstChild.Delete())
            {
                fill_tree_lazy(e.node);
            }
        }
    }
    else
    {
    }
}

// 性能计时相关方法实现
void Viewer::start_timing()
{
    m_start_time = std::chrono::high_resolution_clock::now();
}

void Viewer::log_load_time()
{
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(m_load_time - m_start_time);
    double ms = duration.count() / 1000.0;

    // 在控制台输出加载时间
    std::wstring message = L"文件加载耗时: " + std::to_wstring(ms) + L" ms";
    OutputDebugStringW((message + L"\n").c_str());
}

void Viewer::log_parse_time()
{
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(m_parse_time - m_load_time);
    double ms = duration.count() / 1000.0;

    // 在控制台输出解析时间
    std::wstring message = L"JSON解析耗时: " + std::to_wstring(ms) + L" ms";
    OutputDebugStringW((message + L"\n").c_str());
}

void Viewer::log_display_time()
{
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - m_start_time);
    auto display_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - m_parse_time);
    auto parse_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(m_parse_time - m_load_time);

    double total_ms   = total_duration.count() / 1000.0;
    double display_ms = display_duration.count() / 1000.0;
    double parse_ms   = parse_duration.count() / 1000.0;

    // 在控制台输出显示时间和总时间
    std::wstring display_message = L"树形显示耗时: " + std::to_wstring(display_ms) + L" ms";
    std::wstring total_message   = L"总耗时: " + std::to_wstring(total_ms) + L" ms";

    OutputDebugStringW((display_message + L"\n").c_str());
    OutputDebugStringW((total_message + L"\n").c_str());
    OutputDebugStringW(L"========================================\n");

    // 在窗口标题显示解析时间和总耗时
    std::wstring title = L"JSON Viewer - 处理完成 (解析: " + std::to_wstring(parse_ms) +
                         L" ms, 总计: " + std::to_wstring(total_ms) + L" ms)";
    Text = title;
}