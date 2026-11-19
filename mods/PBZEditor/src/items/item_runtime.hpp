/** @file item_runtime.hpp
 *  @brief Game-thread inventory adapter. Only owned values survive a callback.
 */
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <items/item_editor.hpp>

namespace RC::Unreal {
class UEngine;
}

namespace pbz::items {
/** @brief Concrete item feature runtime; not a general reflection dispatcher. */
class ItemRuntime {
public:
    /** @brief Bind the sole value/request owner shared with the UI. */
    explicit ItemRuntime(std::shared_ptr<EditorState> state) : state_(std::move(state)) {}
    /** @brief Execute only explicit work, with a per-frame refresh budget. */
    auto tick(RC::Unreal::UEngine* engine) noexcept -> void;
    /** @brief Invalidate refresh work and value requests before a map transition. */
    auto begin_world_change() noexcept -> void;

private:
    struct CatalogScan {
        std::uint64_t generation{};
        std::wstring world;
        std::chrono::steady_clock::time_point started;
        std::wstring backpack;
        std::size_t source{};
        std::size_t next{};
        bool source_ready{};
        bool schema_checked{};
        bool registry_checked{};
        bool description_checked{};
        bool ownership_checked{};
        std::optional<std::vector<std::string>> owned_ids;
        std::optional<CatalogItem> pending_row;
        std::vector<std::wstring> keys;
        Catalog result;
    };
    std::optional<CatalogScan> catalog_;
    auto start_catalog(RC::Unreal::UEngine* engine, const Request& request) -> void;
    auto continue_catalog(RC::Unreal::UEngine* engine) -> void;
    struct Refresh {
        std::uint64_t generation{};
        std::wstring world;
        std::vector<std::wstring> objects;
        std::size_t next{};
        std::vector<Item> rows;
    };
    auto start_refresh(RC::Unreal::UEngine* engine, const Request& request) -> void;
    auto continue_refresh(RC::Unreal::UEngine* engine) -> void;
    auto grant_item(RC::Unreal::UEngine* engine, const Request& request) -> void;
    auto apply_quantity(RC::Unreal::UEngine* engine, const Request& request) -> void;

    std::shared_ptr<EditorState> state_;
    std::optional<Refresh> refresh_;
};
}  // namespace pbz::items
