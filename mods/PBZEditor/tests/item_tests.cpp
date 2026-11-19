/** @file item_tests.cpp
 *  @brief Deterministic quantity, ownership and world-generation handoff tests.
 */
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <items/item_editor.hpp>

namespace {
using namespace pbz::items;

auto require(const bool result, const char* explanation) -> void {
    if (!result) {
        throw std::runtime_error(explanation);
    }
}

[[nodiscard]] auto ordinary() -> Item {
    Item item;
    item.id = "material";
    item.name = "Material";
    item.object_path = L"world.backpack.material";
    item.count = 5;
    item.historical_max = 8;
    item.detail = 1;
    item.material_type = 1;
    return item;
}

auto ready(EditorState& state) -> void {
    Snapshot snapshot;
    snapshot.generation = state.snapshot()->generation;
    snapshot.phase = Phase::ready;
    snapshot.world = L"world";
    snapshot.items.push_back(ordinary());
    state.publish(std::move(snapshot));
}

template <typename Test>
auto scenario(const char* name, Test test) -> void {
    test();
    std::cout << "PASS " << name << '\n';
}
}  // namespace

auto main() -> int {
    try {
        scenario("increase decrease and unchanged decisions", [] {
            const auto item = ordinary();
            require(decide(item, 5, 9) == Decision::increase, "increase");
            require(decide(item, 5, 1) == Decision::decrease, "lower bound");
            require(decide(item, 5, 5) == Decision::unchanged, "unchanged");
            require(decide(item, 5, kMaximumQuantity) == Decision::increase, "upper bound");
        });
        scenario("invalid and overflowing quantities refuse", [] {
            for (const auto value :
                 {0, -1, kMaximumQuantity + 1, std::numeric_limits<std::int32_t>::max(),
                  std::numeric_limits<std::int32_t>::min()}) {
                require(decide(ordinary(), 5, value) == Decision::reject, "invalid quantity");
            }
        });
        scenario("game consumption invalidates displayed original", [] {
            auto current = ordinary();
            current.count = 4;
            require(decide(current, 5, 10) == Decision::reject, "stale original");
        });
        scenario("quest attribute progression and conversion items refuse", [] {
            for (const auto detail : {0, 2, 5, 6}) {
                auto item = ordinary();
                item.detail = static_cast<std::uint8_t>(detail);
                require(!editable(item), "special detail");
            }
            auto item = ordinary();
            item.type = 1;
            require(!editable(item), "book");
            item = ordinary();
            item.material_type = 7;
            require(!editable(item), "progression");
            item = ordinary();
            item.integration_count = 10;
            require(!editable(item), "auto integration");
            item = ordinary();
            item.integration_target = true;
            require(!editable(item), "integration target");
            item = ordinary();
            item.adds_attribute = true;
            require(!editable(item), "attribute acquisition");
            item = ordinary();
            item.count = 0;
            require(!editable(item), "new acquisition not supported");
        });
        scenario("rollback never overwrites values outside owned change", [] {
            const auto before = ordinary();
            auto current = before;
            current.count = 7;
            require(rollback_owned(before, current, 10), "partial increase recoverable");
            current.count = 11;
            require(!rollback_owned(before, current, 10), "unowned increase");
            current.count = 4;
            require(!rollback_owned(before, current, 10), "unowned decrease");
            current.count = 10;
            current.historical_max = 20;
            require(!rollback_owned(before, current, 10), "unowned historical value");
            current = before;
            current.adds_attribute = true;
            require(!rollback_owned(before, current, 10), "changed acquisition behavior");
            current = before;
            current.object_path = L"different.object";
            require(!rollback_owned(before, current, 10), "different entity");
        });
        scenario("one request carries exact selected original", [] {
            EditorState state;
            ready(state);
            const auto shown = state.snapshot();
            require(state.apply(*shown, "material", 12), "accepted");
            require(!state.apply(*shown, "material", 13), "busy refuses second request");
            const auto request = state.take_request();
            require(request && request->original.count == 5 && request->target == 12 &&
                        request->world == L"world",
                    "request identity");
            require(!state.has_request() && !state.take_request(), "one consumption");
            require(shown->phase == Phase::ready && shown->items.front().count == 5,
                    "published snapshot immutable");
        });
        scenario("stale UI revision refuses without queuing", [] {
            EditorState state;
            ready(state);
            const auto old = state.snapshot();
            ready(state);
            require(!state.apply(*old, "material", 6) && !state.has_request(), "stale revision");
        });
        scenario("map transition clears requests and blocks loading writes", [] {
            EditorState state;
            ready(state);
            const auto old = state.snapshot();
            require(state.apply(*old, "material", 10), "queued before transition");
            state.begin_world_change();
            require(!state.has_request() && !state.take_request(), "queue cleared");
            require(!state.refresh() && !state.apply(*old, "material", 10), "loading gate");
            auto stale = *old;
            state.publish(std::move(stale));
            require(
                state.snapshot()->generation != old->generation && state.snapshot()->items.empty(),
                "old publication refused");
            state.finish_world_change();
            require(state.refresh(), "explicit refresh after map completion");
        });
        scenario("unverified recovery survives refresh and world change", [] {
            EditorState state;
            ready(state);
            auto failed = *state.snapshot();
            failed.recovery = Recovery{L"world", ordinary(), 30};
            state.publish(std::move(failed));
            require(state.snapshot()->phase == Phase::blocked, "blocked");
            require(!state.apply(*state.snapshot(), "material", 10), "no repeat-click bypass");
            require(state.refresh(), "readonly refresh allowed");
            static_cast<void>(state.take_request());
            ready(state);
            require(
                state.snapshot()->recovery.has_value() && state.snapshot()->phase == Phase::blocked,
                "refresh cannot clear recovery");
            state.begin_world_change();
            state.finish_world_change();
            require(state.snapshot()->recovery.has_value(), "record retained across transition");
        });
        scenario("close rejects new work and retains in-flight failed recovery", [] {
            EditorState state;
            ready(state);
            auto result = *state.snapshot();
            require(state.apply(*state.snapshot(), "material", 10), "queued");
            state.close();
            require(state.closed() && !state.has_request() && !state.refresh(), "closed gate");
            result.recovery = Recovery{L"world", ordinary(), 10};
            state.publish(std::move(result));
            require(state.snapshot()->recovery.has_value(), "in-flight record retained");
        });
        scenario("in-flight recovery survives a nested world transition", [] {
            EditorState state;
            ready(state);
            auto old = *state.snapshot();
            old.recovery = Recovery{L"world", ordinary(), 10};
            state.begin_world_change();
            state.finish_world_change();
            const auto generation = state.snapshot()->generation;
            state.publish(std::move(old));
            require(state.snapshot()->generation == generation, "current world retained");
            require(state.snapshot()->recovery.has_value(), "old recovery retained");
            require(state.snapshot()->phase == Phase::blocked, "writes remain blocked");
        });
        scenario("catalog filter uses category, item ID, localized name and proven absence", [] {
            CatalogItem row{.category = Category::medicine,
                            .ownership = Ownership::unknown,
                            .id = "medicine_id",
                            .name = "药葫芦"};
            require(catalog_matches(row, Category::medicine, "葫芦", false), "localized match");
            require(catalog_matches(row, std::nullopt, "medicine_id", false), "ID match");
            require(!catalog_matches(row, Category::accessory, "", false), "wrong category");
            require(!catalog_matches(row, std::nullopt, "", true), "unknown is not unowned");
            row.ownership = Ownership::unowned;
            require(catalog_matches(row, std::nullopt, "", true), "confirmed absence match");
        });
        scenario("catalog request follows the world generation and preserves backpack", [] {
            EditorState state;
            ready(state);
            require(state.refresh(true), "catalog queued");
            require(!state.refresh(), "concurrent backpack request rejected");
            const auto request = state.take_request();
            require(request && request->kind == Request::Kind::catalog, "catalog request kind");
            require(state.snapshot()->items.size() == 1, "backpack preserved while reading");
            auto result = *state.snapshot();
            result.catalog = std::make_shared<const Catalog>();
            state.begin_world_change();
            state.finish_world_change();
            state.publish(std::move(result));
            require(!state.snapshot()->catalog, "old generation catalog rejected");
        });
        scenario("a verified unowned catalog row queues a single world-bound grant", [] {
            EditorState state;
            auto next = *state.snapshot();
            auto catalog = std::make_shared<Catalog>();
            catalog->world = L"world";
            catalog->backpack = L"backpack";
            CatalogItem row{.category = Category::items,
                            .ownership = Ownership::unknown,
                            .id = "material_a",
                            .row_key = "row_a",
                            .grantable_definition = true};
            require(!eligible_for_grant(row), "unknown ownership");
            row.ownership = Ownership::owned;
            require(!eligible_for_grant(row), "already owned");
            row.ownership = Ownership::unowned;
            require(eligible_for_grant(row), "supported absent material");
            row.category = Category::weapon;
            for (auto category : {Category::weapon, Category::shaqi, Category::medicine,
                                  Category::accessory, Category::book}) {
                row.category = category;
                require(eligible_for_grant(row), "eligible category");
                row.grantable_definition = false;
                require(!eligible_for_grant(row), "ambiguous or unsupported definition");
                row.grantable_definition = true;
            }
            row.category = Category::items;
            catalog->rows.push_back(row);
            row.id = "material_b";
            row.row_key = "row_b";
            catalog->rows.push_back(row);

            next.catalog = std::move(catalog);
            const auto stale = state.snapshot();
            state.publish(std::move(next));
            require(!state.grant(*stale, 1, GrantMode::inventory_only), "stale snapshot rejected");
            const auto shown = state.snapshot();
            require(!state.grant(*shown, 1, GrantMode::save_confirmed_slot),
                    "unconfirmed save slot refused");
            require(!state.grant(*shown, 2, GrantMode::inventory_only), "missing row refused");
            require(state.grant(*shown, 1, GrantMode::inventory_only),
                    "backpack refresh is optional");
            require(!state.grant(*shown, 1, GrantMode::inventory_only), "duplicate click rejected");
            const auto request = state.take_request();
            require(request && request->kind == Request::Kind::grant &&
                        request->grant_mode == GrantMode::inventory_only &&
                        request->world == L"world" && request->row_index == 1 &&
                        request->backpack == L"backpack",
                    "world and backpack carried");
        });
        std::cout << "14 scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
