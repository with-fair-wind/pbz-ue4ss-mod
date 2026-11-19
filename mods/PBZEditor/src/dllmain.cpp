/** @file dllmain.cpp
 *  @brief Mod lifetime, bounded game-thread item work and value-only GUI handoff.
 */
#include <array>
#include <memory>
#include <stdexcept>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Windows.h>
#include <imgui.h>
#include <items/item_editor.hpp>

#include "items/item_runtime.hpp"
#include "items/item_ui.hpp"

namespace {
using namespace RC::Unreal;

/** Keep callback/destructor code mapped while upstream asynchronously collects closures. */
[[nodiscard]] auto pin_module() noexcept -> bool {
    HMODULE module{};
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&pin_module), &module) != FALSE;
}

auto lifetime_error() noexcept -> void {
    try {
        RC::Output::send<RC::LogLevel::Error>(
            STR("PBZEditor: item callbacks/recovery retained; no further writes permitted.\n"));
    } catch (...) {
        // Diagnostics must not escape the engine's unload boundary.
    }
}

class PBZEditorMod final : public RC::CppUserModBase {
public:
    PBZEditorMod()
        : state_(std::make_shared<pbz::items::EditorState>()),
          runtime_(std::make_shared<pbz::items::ItemRuntime>(state_)),
          ui_(std::make_shared<pbz::items::ItemUi>()) {
        if (!pin_module()) {
            throw std::runtime_error("Cannot pin PBZEditor callback code.");
        }
        ModName = STR("PBZEditor");
        ModVersion = STR("0.2.0");
        ModDescription = STR("Phantom Blade Zero inventory quantity editor");
        ModAuthors = STR("Local development");
    }

    ~PBZEditorMod() override {
        static_cast<void>(shutdown());
    }

    auto on_unreal_init() -> void override {
        const auto runtime = runtime_;
        const auto state = state_;
        try {
            callbacks_[0] = Hook::RegisterEngineTickPreCallback(
                [runtime](Hook::TCallbackIterationData<void>&, UEngine* engine, float, bool) {
                    runtime->tick(engine);
                },
                {.bOnce = false,
                 .bReadonly = true,
                 .OwnerModName = STR("PBZEditor"),
                 .HookName = STR("ItemRequests")});
            callbacks_[1] = Hook::RegisterLoadMapPreCallback(
                [runtime](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL,
                          UPendingNetGame*, FString&) { runtime->begin_world_change(); },
                {.bOnce = false,
                 .bReadonly = true,
                 .OwnerModName = STR("PBZEditor"),
                 .HookName = STR("ItemWorldBegin")});
            callbacks_[2] = Hook::RegisterLoadMapPostCallback(
                [state](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL,
                        UPendingNetGame*, FString&) {
                    try {
                        state->finish_world_change();
                    } catch (...) {
                        state->close();
                    }
                },
                {.bOnce = false,
                 .bReadonly = true,
                 .OwnerModName = STR("PBZEditor"),
                 .HookName = STR("ItemWorldReady")});
            for (const auto id : callbacks_) {
                if (id == Hook::ERROR_ID) {
                    throw std::runtime_error("Missing item lifetime callback.");
                }
            }
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("PBZEditor loaded (v0.2.0); item work is explicit, no idle reflection.\n"));
        } catch (...) {
            static_cast<void>(shutdown());
            lifetime_error();
        }
    }

    auto on_ui_init() -> void override {
        UE4SS_ENABLE_IMGUI();
        if (!GUITabs.empty()) {
            return;
        }
        register_tab(STR("PBZEditor"), [](RC::CppUserModBase* base) {
            auto* mod = static_cast<PBZEditorMod*>(base);
            const auto state = mod->state_;
            const auto ui = mod->ui_;
            if (state->closed()) {
                return;
            }
            try {
                ui->render(*state);
            } catch (...) {
                state->close();
                lifetime_error();
            }
        });
        RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor GUI registered.\n"));
    }

    /** Stop inputs before unregistering; failed unregister IDs and recovery records are retained.
     */
    [[nodiscard]] auto shutdown() noexcept -> bool {
        state_->close();
        bool clean = true;
        // Remove tabs while derived members still exist; the host's tab mutex drains rendering.
        for (auto& tab : GUITabs) {
            if (!tab) {
                continue;
            }
            try {
                RC::UE4SSProgram::get_program().remove_gui_tab(tab);
                tab.reset();
            } catch (...) {
                clean = false;
            }
        }
        if (clean) {
            GUITabs.clear();
        }
        for (auto& id : callbacks_) {
            if (id == Hook::ERROR_ID) {
                continue;
            }
            try {
                if (Hook::UnregisterCallback(id)) {
                    id = Hook::ERROR_ID;
                } else {
                    clean = false;
                }
            } catch (...) {
                clean = false;
            }
        }
        try {
            clean = clean && !state_->snapshot()->recovery.has_value();
        } catch (...) {
            clean = false;
        }
        return clean;
    }

private:
    std::shared_ptr<pbz::items::EditorState> state_;
    std::shared_ptr<pbz::items::ItemRuntime> runtime_;
    std::shared_ptr<pbz::items::ItemUi> ui_;
    std::array<Hook::GlobalCallbackId, 3> callbacks_{};
};
}  // namespace

/** @brief Construct the host-owned mod; never propagate construction errors into UE4SS. */
extern "C" __declspec(dllexport) auto start_mod() noexcept -> RC::CppUserModBase* {
    try {
        return new PBZEditorMod{};
    } catch (...) {
        lifetime_error();
        return nullptr;
    }
}

/** @brief Destroy only after callbacks drain and any pending recovery is accounted for. */
extern "C" __declspec(dllexport) auto uninstall_mod(RC::CppUserModBase* base) noexcept -> void {
    auto* mod = static_cast<PBZEditorMod*>(base);
    if (mod == nullptr) {
        return;
    }
    if (!mod->shutdown()) {
        lifetime_error();
        return;  // Pinned instance retains the record and inert callback captures until exit.
    }
    delete mod;
}
