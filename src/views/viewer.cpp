#include "viewer.hpp"
#include "simdjson.h"

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

    try
    {
        const auto file = sw::Utils::ToMultiByteStr(m_file);
        m_json          = simdjson::padded_string::load(file);
        fill_tree_root();
    }
    catch (...)
    {
    }

    m_parsing = false;
}

void Viewer::fill_tree_root()
{
    simdjson::ondemand::document doc        = m_parser.iterate(m_json);
    auto                         root_value = doc.get_value();

    switch (root_value.type())
    {
    case simdjson::ondemand::json_type::object: {
        auto root_obj = root_value.get_object();
        for (auto field : root_obj)
        {
            std::string_view key_view = field.unescaped_key();
            std::wstring     key      = sw::Utils::ToWideStr(std::string(key_view));
            auto             node     = m_treeView.AddItem(key);

            if (field.has_value())
            {
                simdjson::ondemand::value value = field.value().value();
                process_value(node, value);
            }
        }
    }
    break;
    case simdjson::ondemand::json_type::array: {
        auto root_array = root_value.get_array();
        for (auto value : root_array)
        {
            // auto node = m_treeView.AddNode(parent, "");
            // process_value(node, value);
        }
    }
    }
}

void Viewer::process_value(sw::TreeViewNode &parent, simdjson::ondemand::value value)
{
    auto type = value.type();
    switch (type)
    {
    case simdjson::ondemand::json_type::object: {
        int count = value.get_object().count_fields();
        // auto item = m_treeView.AddItem(L"");
        // item.SetUserData();
    }
    }
}

void Viewer::on_tree_view_item_expanded(sw::UIElement &sender, sw::TreeViewItemExpandedEventArgs &e)
{
}