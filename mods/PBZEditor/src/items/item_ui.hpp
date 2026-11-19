/** @file item_ui.hpp
 *  @brief ImGui inputs and presentation of immutable inventory values.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <items/item_editor.hpp>

namespace pbz::items {
/** @brief UI-only filter, selection and uncommitted quantity input. */
struct ItemUi {
    std::array<char, 128> filter{};
    std::string selected;
    std::array<char, 128> catalog_filter{};
    int catalog_category{};
    bool only_unowned{};
    std::optional<std::size_t> catalog_selection;
    std::weak_ptr<const Catalog> displayed_catalog;
    std::vector<std::size_t> visible_catalog_rows;
    auto render_catalog(EditorState& state, const Snapshot& snapshot) -> void;
    auto render_backpack(EditorState& state, const std::shared_ptr<const Snapshot>& snapshot)
        -> void;
    std::uint64_t revision{};
    std::int32_t target{1};
    /** @brief Render values and enqueue requests; never call Unreal from this method. */
    auto render(EditorState& state) -> void;
};
}  // namespace pbz::items
