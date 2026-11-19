/** @file item_ui.cpp
 *  @brief Inventory quantity UI with explicit refresh and apply controls.
 */
#include "item_ui.hpp"

#include <algorithm>
#include <string_view>

#include <imgui.h>

namespace pbz::items {
auto ItemUi::render(EditorState& state) -> void {
    const auto snapshot = state.snapshot();
    if (state.closed()) {
        return;
    }
    if (!ImGui::BeginTabBar("PBZItemViews")) {
        return;
    }
    if (ImGui::BeginTabItem("物品目录")) {
        render_catalog(state, *snapshot);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("已有背包")) {
        render_backpack(state, snapshot);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

auto ItemUi::render_catalog(EditorState& state, const Snapshot& snapshot) -> void {
    constexpr std::array categories{"全部", "道具与材料", "主武器", "影之武装",
                                    "药品", "饰品",       "书籍"};
    constexpr std::array ownership{"待确认", "已持有", "未持有"};
    ImGui::BeginDisabled(snapshot.phase == Phase::busy || snapshot.phase == Phase::transitioning);
    if (ImGui::Button("刷新目录")) {
        static_cast<void>(state.refresh(true));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", snapshot.catalog_message.c_str());
    bool changed = ImGui::Combo("分类", &catalog_category, categories.data(),
                                static_cast<int>(categories.size()));
    changed |= ImGui::InputText("搜索名称 / ID", catalog_filter.data(), catalog_filter.size());
    changed |= ImGui::Checkbox("仅未持有", &only_unowned);
    if (displayed_catalog.lock() != snapshot.catalog || !snapshot.catalog) {
        displayed_catalog = snapshot.catalog;
        catalog_selection.reset();
        changed = true;
    }
    if (!snapshot.catalog) {
        visible_catalog_rows.clear();
        return;
    }
    const auto& rows = snapshot.catalog->rows;
    if (changed) {
        visible_catalog_rows.clear();
        const auto category = catalog_category == 0
                                  ? std::nullopt
                                  : std::optional{static_cast<Category>(catalog_category - 1)};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (catalog_matches(rows[index], category, catalog_filter.data(), only_unowned)) {
                visible_catalog_rows.push_back(index);
            }
        }
        if (catalog_selection &&
            !std::ranges::binary_search(visible_catalog_rows, *catalog_selection)) {
            catalog_selection.reset();
        }
    }
    ImGui::Text("显示 %zu / %zu 项", visible_catalog_rows.size(), rows.size());
    if (ImGui::BeginTable("PBZCatalog", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2{0, 350})) {
        ImGui::TableSetupColumn("物品");
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("分类");
        ImGui::TableSetupColumn("持有状态");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible_catalog_rows.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto row_index = visible_catalog_rows[index];
                const auto& item = rows[row_index];
                ImGui::PushID(index);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable("##row", catalog_selection == row_index,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    catalog_selection = row_index;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(item.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(item.id.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(categories[static_cast<std::size_t>(item.category) + 1]);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(ownership[static_cast<std::size_t>(item.ownership)]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (catalog_selection) {
        const auto& item = rows[*catalog_selection];
        ImGui::TextWrapped("%s", item.name.c_str());
        if (!item.description.empty()) {
            ImGui::TextWrapped("%s", item.description.c_str());
        }
        ImGui::TextWrapped("来源 %s · %s", item.source.c_str(), item.row_key.c_str());
        if (item.category == Category::book && !item.grantable_definition &&
            item.ownership == Ownership::unowned) {
            ImGui::TextWrapped("此条目没有唯一可发放的整书道具。可在书籍分类中选择实际背包道具。");
        }
        if (eligible_for_grant(item)) {
            ImGui::BeginDisabled(snapshot.recovery.has_value() || snapshot.phase == Phase::busy ||
                                 snapshot.phase == Phase::transitioning ||
                                 snapshot.phase == Phase::blocked);
            if (ImGui::Button("仅获得")) {
                static_cast<void>(
                    state.grant(snapshot, *catalog_selection, GrantMode::inventory_only));
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(snapshot.catalog->slot.empty());
            if (ImGui::Button("获得 1 个并保存")) {
                static_cast<void>(
                    state.grant(snapshot, *catalog_selection, GrantMode::save_confirmed_slot));
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (snapshot.catalog->slot.empty()) {
                ImGui::TextWrapped("仅获得后请使用游戏内存档。当前载入槽与继续槽尚未确认一致。");
            } else {
                ImGui::Text("保存槽位 %s", snapshot.catalog->slot_label.c_str());
            }
        }
    }
}

auto ItemUi::render_backpack(EditorState& state, const std::shared_ptr<const Snapshot>& snapshot)
    -> void {
    ImGui::TextUnformatted("物品数量");
    ImGui::Separator();
    ImGui::TextWrapped("仅编辑已持有的普通可堆叠道具。任务、属性增长及自动合成条目保持只读。");
    ImGui::BeginDisabled(snapshot->phase == Phase::busy || snapshot->phase == Phase::transitioning);
    if (ImGui::Button("读取背包")) {
        static_cast<void>(state.refresh());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", snapshot->message.c_str());
    if (snapshot->recovery) {
        ImGui::TextWrapped("原值记录：%s，数量 %d，历史值 %d。恢复未确认，后续写入已停用。",
                           snapshot->recovery->original.id.c_str(),
                           snapshot->recovery->original.count,
                           snapshot->recovery->original.historical_max);
    }
    ImGui::InputText("搜索名称 / ID", filter.data(), filter.size());
    ImGui::Text("已读取 %zu 项；修改范围 1–%d。", snapshot->items.size(), kMaximumQuantity);
    if (revision != snapshot->revision) {
        const auto current = std::ranges::find(snapshot->items, selected, &Item::id);
        if (current != snapshot->items.end()) {
            target = current->count;
        } else {
            selected.clear();
        }
        revision = snapshot->revision;
    }
    if (ImGui::BeginTable("items", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2{0, 390})) {
        ImGui::TableSetupColumn("物品");
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("数量");
        ImGui::TableSetupColumn("编辑状态");
        ImGui::TableHeadersRow();
        for (const auto& item : snapshot->items) {
            const auto query = std::string_view{filter.data()};
            if (!query.empty() && item.id.find(query) == std::string::npos &&
                item.name.find(query) == std::string::npos) {
                continue;
            }
            ImGui::PushID(item.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(item.name.c_str(), selected == item.id,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selected = item.id;
                target = item.count;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(item.id.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", item.count);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(editable(item) ? "可修改" : "特殊道具 / 只读");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto found = std::ranges::find(snapshot->items, selected, &Item::id);
    if (found == snapshot->items.end()) {
        ImGui::TextUnformatted("选择一项道具后输入目标数量。");
        return;
    }
    ImGui::Text("当前选择：%s（%s），数量 %d。", found->name.c_str(), found->id.c_str(),
                found->count);
    ImGui::InputInt("目标数量", &target);
    const bool can_apply = snapshot->phase == Phase::ready && !snapshot->recovery &&
                           decide(*found, found->count, target) != Decision::reject;
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("应用数量")) {
        static_cast<void>(state.apply(*snapshot, selected, target));
    }
    ImGui::EndDisabled();
}
}  // namespace pbz::items
