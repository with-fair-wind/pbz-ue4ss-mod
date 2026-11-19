/**
 * @file item_editor.hpp
 * @brief Value-only inventory snapshots, quantity decisions and UI/game-thread handoff.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbz::items {
inline constexpr std::int32_t kMaximumQuantity = 999999;
inline constexpr std::size_t kMaximumItems = 4096;
/** @brief Reflected item facts converted to owned standard-library values. */
struct Item {
    std::string id;
    std::string name;
    std::wstring object_path;
    std::int32_t count{};
    std::int32_t historical_max{};
    std::uint8_t type{};
    std::uint8_t detail{};
    std::uint8_t material_type{};
    std::int32_t integration_count{};
    bool integration_target{};
    bool adds_attribute{};
};

/** @brief Offer only owned ordinary stacks without acquisition/conversion side effects. */
[[nodiscard]] inline auto editable(const Item& item) noexcept -> bool {
    const bool ordinary_detail = item.detail == 1 || item.detail == 3 || item.detail == 4;
    return item.type == 0 && ordinary_detail && item.material_type != 7 &&
           item.integration_count == 0 && !item.integration_target && !item.adds_attribute &&
           item.count > 0 && item.count <= kMaximumQuantity && !item.id.empty();
}

/** @brief Result of validating a quantity request against freshly read values. */
enum class Decision { reject, unchanged, increase, decrease };

/** @brief Limit rollback to values inside the change this transaction could have caused. */
[[nodiscard]] inline auto rollback_owned(const Item& original, const Item& current,
                                         const std::int32_t requested) noexcept -> bool {
    return original.id == current.id && original.object_path == current.object_path &&
           original.type == current.type && original.detail == current.detail &&
           original.material_type == current.material_type &&
           original.adds_attribute == current.adds_attribute &&
           original.integration_count == current.integration_count &&
           original.integration_target == current.integration_target &&
           current.count >= std::min(original.count, requested) &&
           current.count <= std::max(original.count, requested) &&
           current.historical_max >= original.historical_max &&
           current.historical_max <= std::max(original.historical_max, requested);
}

/** @brief Validate before computing a bounded native delta; never clamp user input silently. */
[[nodiscard]] inline auto decide(const Item& actual, const std::int32_t expected,
                                 const std::int32_t target) noexcept -> Decision {
    if (!editable(actual) || actual.count != expected || target < 1 || target > kMaximumQuantity) {
        return Decision::reject;
    }
    return target == actual.count  ? Decision::unchanged
           : target > actual.count ? Decision::increase
                                   : Decision::decrease;
}

/** @brief Interface phase; failures with an unverified recovery cannot accept further edits. */
enum class Phase { unavailable, ready, busy, blocked, transitioning };

/** @brief Original values retained when a failed write cannot be recovered and verified. */
struct Recovery {
    std::wstring world;
    Item original;
    std::int32_t requested{};
};

/** @brief Category derived from a verified item table. */
enum class Category { items, weapon, shaqi, medicine, accessory, book };
/** @brief Ownership as reported by the supported query for the item's category. */
enum class Ownership { unknown, owned, unowned };

/** @brief One exact source row converted to independent display values. */
struct CatalogItem {
    Category category{};
    Ownership ownership{Ownership::unknown};
    std::string id;
    std::string name;
    std::string description;
    std::string source;
    std::string row_key;
    bool grantable_definition{};
};

/** @brief Offer only unowned items with an unambiguous supported catalog definition. */
[[nodiscard]] inline auto eligible_for_grant(const CatalogItem& row) noexcept -> bool {
    return row.ownership == Ownership::unowned && row.grantable_definition && !row.id.empty() &&
           !row.row_key.empty();
}

/** @brief Catalog and coverage for a specific world and backpack. */
struct Catalog {
    std::wstring world;
    std::wstring backpack;
    std::wstring slot;
    std::string slot_label;
    std::vector<CatalogItem> rows;
    std::size_t raw{};
    std::size_t excluded{};
    std::size_t aliases{};
    std::size_t failed_rows{};
    std::size_t failed_sources{};
    std::size_t registry_tables{};
};

[[nodiscard]] inline auto catalog_matches(const CatalogItem& item, std::optional<Category> category,
                                          std::string_view query, bool only_unowned) -> bool {
    return (!category || *category == item.category) &&
           (!only_unowned || item.ownership == Ownership::unowned) &&
           (item.id.find(query) != std::string::npos || item.name.find(query) != std::string::npos);
}

/** @brief Immutable published data; it contains no engine handles. */
struct Snapshot {
    std::uint64_t generation{};
    std::uint64_t revision{};
    Phase phase{Phase::unavailable};
    std::wstring world;
    std::vector<Item> items;
    std::shared_ptr<const Catalog> catalog;
    std::string catalog_message{"进入存档后点击“刷新目录”。"};
    std::string message{"进入存档后点击“读取背包”。"};
    std::optional<Recovery> recovery;
};

/** @brief A single explicit refresh or edit captured from the displayed snapshot. */
enum class GrantMode { inventory_only, save_confirmed_slot };

struct Request {
    enum class Kind { refresh, apply, catalog, grant };
    Kind kind{Kind::refresh};
    std::uint64_t generation{};
    std::wstring world;
    Item original;
    std::int32_t target{};
    std::wstring backpack;
    std::size_t row_index{};
    GrantMode grant_mode{GrantMode::inventory_only};
};

/** @brief Sole owner of published values, generation, requests and retained recovery records. */
class EditorState {
public:
    /** @brief Obtain an immutable snapshot without copying its item array. */
    [[nodiscard]] auto snapshot() const -> std::shared_ptr<const Snapshot> {
        const std::lock_guard lock(mutex_);
        return snapshot_;
    }

    /** @brief Queue one explicit refresh; blocked recovery is retained through refreshes. */
    auto refresh(bool catalog = false) -> bool {
        const std::lock_guard lock(mutex_);
        if (closed() || snapshot_->phase == Phase::busy ||
            snapshot_->phase == Phase::transitioning) {
            return false;
        }
        request_ = Request{.generation = snapshot_->generation};
        auto next = *snapshot_;
        next.phase = Phase::busy;
        if (catalog) {
            request_->kind = Request::Kind::catalog;
            next.catalog_message = "正在读取物品目录…";
        } else {
            next.message = "正在读取背包…";
        }
        publish_locked(std::move(next));
        pending_.store(true, std::memory_order_release);
        return true;
    }

    /** @brief Reject stale displays and queue the selected item identity and original count. */
    auto apply(const Snapshot& shown, const std::string& id, const std::int32_t target) -> bool {
        const std::lock_guard lock(mutex_);
        if (closed() || snapshot_->phase != Phase::ready || snapshot_->recovery ||
            shown.generation != snapshot_->generation || shown.revision != snapshot_->revision) {
            return false;
        }
        const auto found = std::ranges::find(snapshot_->items, id, &Item::id);
        if (found == snapshot_->items.end() ||
            decide(*found, found->count, target) == Decision::reject) {
            return false;
        }
        request_ = Request{Request::Kind::apply, shown.generation, shown.world, *found, target};
        auto next = *snapshot_;
        next.phase = Phase::busy;
        next.message = "正在修改并重读验证…";
        publish_locked(std::move(next));
        pending_.store(true, std::memory_order_release);
        return true;
    }

    /** @brief Queue one grant from the current catalog, even before reading the owned backpack. */
    auto grant(const Snapshot& shown, std::size_t row_index, GrantMode mode) -> bool {
        const std::lock_guard lock(mutex_);
        if (closed() || snapshot_->recovery || snapshot_->phase == Phase::busy ||
            snapshot_->phase == Phase::blocked || snapshot_->phase == Phase::transitioning ||
            shown.generation != snapshot_->generation || shown.revision != snapshot_->revision ||
            !snapshot_->catalog || snapshot_->catalog->world.empty() ||
            snapshot_->catalog->backpack.empty() ||
            (mode == GrantMode::save_confirmed_slot && snapshot_->catalog->slot.empty()) ||
            row_index >= snapshot_->catalog->rows.size() ||
            !eligible_for_grant(snapshot_->catalog->rows[row_index])) {
            return false;
        }
        request_ = Request{.kind = Request::Kind::grant,
                           .generation = shown.generation,
                           .world = snapshot_->catalog->world,
                           .backpack = snapshot_->catalog->backpack,
                           .row_index = row_index,
                           .grant_mode = mode};
        auto next = *snapshot_;
        next.phase = Phase::busy;
        next.catalog_message =
            mode == GrantMode::inventory_only ? "正在获得道具…" : "正在获得道具并请求游戏保存…";
        publish_locked(std::move(next));
        pending_.store(true, std::memory_order_release);
        return true;
    }

    /** @brief Atomic idle gate; it performs no engine lookup and takes no idle-frame lock. */
    [[nodiscard]] auto has_request() const noexcept -> bool {
        return !closed() && pending_.load(std::memory_order_acquire);
    }

    /** @brief Check the lifetime gate without touching engine objects or allocating. */
    [[nodiscard]] auto closed() const noexcept -> bool {
        return closed_.load(std::memory_order_acquire);
    }

    /** @brief Consume the single pending request on the game thread. */
    [[nodiscard]] auto take_request() -> std::optional<Request> {
        const std::lock_guard lock(mutex_);
        auto request = std::move(request_);
        request_.reset();
        pending_.store(false, std::memory_order_release);
        return request;
    }

    /** @brief Publish a game-thread result only if its world generation remains current. */
    auto publish(Snapshot next) -> void {
        const std::lock_guard lock(mutex_);
        if (closed() && !next.recovery) {
            return;
        }
        if (next.generation != snapshot_->generation) {
            if (!next.recovery) {
                return;
            }
            auto current = *snapshot_;
            current.recovery = std::move(next.recovery);
            current.phase = Phase::blocked;
            current.message = "切图期间恢复未确认，已保留原值记录并停用修改。";
            publish_locked(std::move(current));
            return;
        }
        if (snapshot_->recovery && !next.recovery) {
            next.recovery = snapshot_->recovery;
        }
        if (next.recovery && next.phase != Phase::busy) {
            next.phase = Phase::blocked;
        }
        publish_locked(std::move(next));
    }

    /** @brief Invalidate queued work and old selections before any map transition. */
    auto begin_world_change() -> void {
        const std::lock_guard lock(mutex_);
        if (closed()) {
            return;
        }
        request_.reset();
        pending_.store(false, std::memory_order_release);
        Snapshot next;
        next.generation = snapshot_->generation + 1;
        next.phase = Phase::transitioning;
        next.recovery = snapshot_->recovery;
        next.message = "世界已切换，请在存档就绪后重新读取背包。";
        publish_locked(std::move(next));
    }

    /** @brief Permit explicit reads again only after the map callback has completed. */
    auto finish_world_change() -> void {
        const std::lock_guard lock(mutex_);
        if (closed() || snapshot_->phase != Phase::transitioning) {
            return;
        }
        auto next = *snapshot_;
        next.phase = Phase::unavailable;
        publish_locked(std::move(next));
    }

    /** @brief Close the handoff before unregistering callbacks; retained records stay owned. */
    auto close() noexcept -> void {
        closed_.store(true, std::memory_order_release);
        pending_.store(false, std::memory_order_release);
        try {
            const std::lock_guard lock(mutex_);
            request_.reset();
        } catch (...) {
            // The atomic gate already prevents both request acceptance and callback execution.
        }
    }

private:
    auto publish_locked(Snapshot next) -> void {
        next.revision = snapshot_->revision + 1;
        snapshot_ = std::make_shared<const Snapshot>(std::move(next));
    }

    mutable std::mutex mutex_;
    std::shared_ptr<const Snapshot> snapshot_{std::make_shared<const Snapshot>()};
    std::optional<Request> request_;
    std::atomic<bool> pending_{};
    std::atomic<bool> closed_{};
};
}  // namespace pbz::items
