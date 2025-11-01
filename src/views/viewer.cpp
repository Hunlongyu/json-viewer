#include "viewer.hpp"
#include "MsgBox.h"

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
    m_grid.SetRows({sw::FillRemainGridRow{}, sw::AutoSizeGridRow{}});
    m_grid.SetColumns({sw::FillRemainGridColumn{}});
    AddChild(m_grid);

    m_treeView.HorizontalAlignment = sw::HorizontalAlignment::Stretch;
    m_treeView.VerticalAlignment   = sw::VerticalAlignment::Stretch;
    m_treeView.SetStyle(WS_BORDER, false);
    m_treeView.SendMessageW(TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    m_grid.AddChild(m_treeView, sw::GridLayoutTag{0, 0});

    DragAcceptFiles(Handle, true);
    RegisterRoutedEvent<sw::DropFilesEventArgs>(*this, &Viewer::drop_file);

    RegisterRoutedEvent<sw::TreeViewItemExpandedEventArgs>(
        *this, &Viewer::on_tree_view_item_expanded
    );

    init_menu();
}

void Viewer::init_menu()
{
    m_menu = new sw::Menu({
        sw::MenuItem(
            L"File (&F)",
            {sw::MenuItem(
                 L"Open (&O)",
                 [this](sw::MenuItem &) {
                     sw::OpenFileDialog dialog;
                     dialog.Title = L"Select a json file.";
                     if (dialog.ShowDialog())
                     {
                         m_startTime = std::chrono::steady_clock::now();
                         m_file      = dialog.FileName.Get();
                         parse_file();
                     }
                 }
             ),
             sw::MenuItem(
                 L"Exit (&X)",
                 [this](sw::MenuItem &) {
                     PostQuitMessage(0);
                 }
             )}
        ),
        sw::MenuItem(L"Edit (&E)", {sw::MenuItem(L"Find (&Ctrl+F)", [](sw::MenuItem &) {})}),
        sw::MenuItem(
            L"Help (&H)",
            {sw::MenuItem(
                 L"Home (&H)",
                 [](sw::MenuItem &) {
                     ShellExecuteW(
                         nullptr,
                         L"open",
                         L"https://github.com/Hunlongyu/json-viewer",
                         nullptr,
                         nullptr,
                         SW_SHOWNORMAL
                     );
                 }
             ),
             sw::MenuItem(L"Author (&A)", [](sw::MenuItem &) {
                 ShellExecuteW(
                     nullptr,
                     L"open",
                     L"https://github.com/Hunlongyu",
                     nullptr,
                     nullptr,
                     SW_SHOWNORMAL
                 );
             })}
        ),
    });
    Menu   = m_menu;

    m_statusBar = new sw::StatusBar();
    m_statusBar->SetParts({200, -1});
    m_statusBar->SetTextAt(0, L"Ready");
    m_grid.AddChild(m_statusBar, sw::GridLayoutTag{1, 0});
}

void Viewer::drop_file(sw::UIElement &sender, sw::DropFilesEventArgs &e)
{
    if (m_dropping.exchange(true))
    {
        return;
    }

    m_startTime = std::chrono::steady_clock::now();
    Text        = L"JSON Viewer";
    m_statusBar->SetTextAt(0, L"Loading & Parsing & Rendering");

    if (m_treeView.AllItemsCount > 0)
    {
        m_treeView.Clear();
        m_treeView.Update();
    }
    struct DrapGuard
    {
        HDROP             hDrop;
        std::atomic_bool &flag;
        ~DrapGuard()
        {
            DragFinish(hDrop);
            flag = false;
        }
    } guard{.hDrop = e.hDrop, .flag = m_dropping};

    if (DragQueryFileW(e.hDrop, -1, nullptr, 0) == 0)
    {
        Text = L"JSON Viewer";
        m_statusBar->SetTextAt(0, L"Ready");
        return;
    }

    std::wstring filePath(MAX_PATH, L'\0');
    DragQueryFileW(e.hDrop, 0, filePath.data(), MAX_PATH);
    m_file = std::move(filePath);

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

    try
    {
        m_json = padded_string::load(sw::Utils::ToMultiByteStr(m_file));
        fill_tree_root();
        const auto filename = get_filename_from_path(m_file);
        Text                = std::format(L"JSON Viewer - {}", filename);
    }
    catch (const std::exception &e)
    {
        Text                = L"JSON Viewer";
        const auto end_time = std::chrono::steady_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - m_startTime);

        const std::wstring message = std::format(L" Took: {}ms", duration.count());
        m_statusBar->SetTextAt(0, message);
    }

    m_parsing = false;
}

void Viewer::fill_tree_root()
{
    if (m_treeView.AllItemsCount > 0)
    {
        m_treeView.Clear();
    }
    try
    {
        document doc               = m_parser.iterate(m_json);
        auto     root_value        = doc.get_value();
        auto     value_type_result = root_value.type();

        if (value_type_result.error() == SUCCESS)
        {
            const auto type = value_type_result.value();
            if (type == json_type::object)
            {
                fill_tree_with_path(root_value.get_object(), m_treeView, "$", doc);
            }
            else if (type == json_type::array)
            {
                fill_tree_with_path(root_value.get_array(), m_treeView, "$", doc);
            }
            else
            {
                m_treeView.AddItem(L"根节点不是对象或数组");
            }
        }

        const auto endTime = std::chrono::steady_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(endTime - m_startTime);
        const std::wstring timeText = std::format(L" Took: {}ms", duration.count());
        m_statusBar->SetTextAt(0, timeText);
    }
    catch (simdjson_error &e)
    {
        sw::MsgBox::ShowWarning(
            sw::Utils::ToWideStr(e.what()), L"JSON Viewer", sw::MsgBoxButton::Ok
        );
    }
    catch (const std::exception &e)
    {
        m_treeView.AddItem(L"[解析失败]");
        sw::MsgBox::ShowWarning(
            sw::Utils::ToWideStr(e.what()), L"JSON Viewer", sw::MsgBoxButton::Ok
        );
    }
}

void Viewer::fill_tree_with_path(
    object object, sw::TreeView &tree_view, const std::string &base_path, const document &doc
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
            const auto   text          = std::format(L"{0}: {1}", key, value_preview);
            const auto   node          = tree_view.AddItem(text);

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
    catch ([[maybe_unused]] simdjson_error &e)
    {
        if (const char *error_msg; doc.current_location().get(error_msg) == SUCCESS)
        {
            size_t      line  = 1;
            const char *start = m_json.data();
            for (const char *p = start; p < error_msg; ++p)
            {
                if (*p == '\n')
                {
                    line++;
                }
            }
            const std::wstring text = L"json parse error at line: " + std::to_wstring(line);
            sw::MsgBox::ShowWarning(text, L"JSON Viewer", sw::MsgBoxButton::Ok);
        }
    }
}

void Viewer::fill_tree_with_path(
    array array, sw::TreeView &tree_view, const std::string &base_path, const document &doc
)
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

            const auto text = std::format(L"{0}: {1}", idx, value_preview);
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
    catch ([[maybe_unused]] simdjson_error &e)
    {
        if (const char *error_msg; doc.current_location().get(error_msg) == SUCCESS)
        {
            size_t      line  = 1;
            const char *start = m_json.data();
            for (const char *p = start; p < error_msg; ++p)
            {
                if (*p == '\n')
                {
                    line++;
                }
            }
            const std::wstring text = L"json parse error at line: " + std::to_wstring(line);
            sw::MsgBox::ShowWarning(text, L"JSON Viewer", sw::MsgBoxButton::Ok);
        }
    }
}

void Viewer::fill_tree_with_path(
    object object, const sw::TreeViewNode &parent, const std::string &base_path, const document &doc
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
            const auto   text          = std::format(L"{0}: {1}", key, value_preview);
            const auto   node          = parent.AddChild(text);

            if (has_children(value))
            {
                const auto current_path = build_object_path(base_path, key_view);
                add_placeholder_child(node, current_path);
            }
        }
    }
    catch ([[maybe_unused]] simdjson_error &e)
    {
        if (const char *error_msg; doc.current_location().get(error_msg) == SUCCESS)
        {
            size_t      line  = 1;
            const char *start = m_json.data();
            for (const char *p = start; p < error_msg; ++p)
            {
                if (*p == '\n')
                {
                    line++;
                }
            }
            const std::wstring text = L"json parse error at line: " + std::to_wstring(line);
            sw::MsgBox::ShowWarning(text, L"JSON Viewer 3", sw::MsgBoxButton::Ok);
        }
    }
}

void Viewer::fill_tree_with_path(
    array array, const sw::TreeViewNode &parent, const std::string &base_path, document &doc
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
                const auto current_path = build_array_path(base_path, idx);
                add_placeholder_child(node, current_path);
            }
            ++idx;
        }
    }
    catch (simdjson_error &e)
    {
        sw::MsgBox::ShowWarning(
            sw::Utils::ToWideStr(e.what()), L"JSON Viewer 4", sw::MsgBoxButton::Ok
        );
    }
}

void Viewer::fill_tree_lazy(const sw::TreeViewNode &node)
{
    try
    {
        const std::string *json_path_ptr = static_cast<std::string *>(node.GetUserData());
        if (json_path_ptr == nullptr)
        {
            return;
        }
        const std::string json_path = *json_path_ptr;
        if (json_path.empty())
        {
            return;
        }

        document doc = m_parser.iterate(m_json);

        auto value_result = doc.at_path(json_path);
        if (value_result.error() != SUCCESS)
        {
            return;
        }

        auto value             = value_result.value();
        auto value_type_result = value.type();
        if (value_type_result.error() != SUCCESS)
        {
            return;
        }

        const auto type = value_type_result.value();
        if (type == json_type::object)
        {
            fill_tree_with_path(value.get_object(), node, json_path, doc);
        }
        else if (type == json_type::array)
        {
            fill_tree_with_path(value.get_array(), node, json_path, doc);
        }
    }
    catch (simdjson_error &e)
    {
        sw::MsgBox::ShowWarning(
            sw::Utils::ToWideStr(e.what()), L"JSON Viewer", sw::MsgBoxButton::Ok
        );
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
                return std::format(L"{{{0}}}", count.value_unsafe());
            }
            return L"{...}";
        }
        case json_type::array: {
            auto       obj   = value.get_array();
            const auto count = obj.count_elements();
            if (count.error() == SUCCESS)
            {
                return std::format(L"[{0}]", count.value_unsafe());
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
    catch (std::exception &e)
    {
        sw::MsgBox::ShowWarning(
            sw::Utils::ToWideStr(e.what()), L"JSON Viewer", sw::MsgBoxButton::Ok
        );
    }
    return L"[未知类型]";
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
    if (parent.AddChild(L"Loading..."))
    {
        parent.SetUserData(new std::string(json_path));
    }
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

std::wstring Viewer::get_filename_from_path(const std::wstring_view path)
{
    try
    {
        return fs::path(path).filename().wstring();
    }
    catch (...)
    {
        const auto pos = path.find_last_of(L"\\/");
        return pos != std::wstring_view::npos ? std::wstring(path.substr(pos + 1))
                                              : std::wstring(path);
    }
}

std::string Viewer::build_object_path(std::string_view base_path, std::string_view key)
{
    if (base_path.empty() || base_path == "$")
    {
        return std::format("$.{}", key);
    }
    return std::format("{}.{}", base_path, key);
}

std::string Viewer::build_array_path(std::string_view base_path, int index)
{
    if (base_path.empty() || base_path == "$")
    {
        return std::format("$[{}]", index);
    }
    return std::format("{}[{}]", base_path, index);
}
