/**
 * @file dllmain.cpp
 * @brief PBZ C++ mod loading and GUI smoke test; no gameplay writes or runtime hooks.
 */
#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <imgui.h>

namespace {
/** @brief Minimal editor entry used to verify the matched UE4SS build and ImGui integration. */
class PBZEditorMod final : public RC::CppUserModBase {
public:
    PBZEditorMod() {
        ModName = STR("PBZEditor");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Phantom Blade Zero development environment smoke test");
        ModAuthors = STR("Local development");
    }

    auto on_unreal_init() -> void override {
        RC::Output::send<RC::LogLevel::Verbose>(
            STR("PBZEditor loaded (v0.1.0); gameplay features are not implemented.\n"));
    }

    auto on_ui_init() -> void override {
        UE4SS_ENABLE_IMGUI();
        // UE4SS can initialize the UI again; the base class owns the registered tabs.
        if (!GUITabs.empty()) {
            return;
        }
        register_tab(STR("PBZEditor"), [](RC::CppUserModBase*) {
            ImGui::TextUnformatted("PBZEditor development environment");
            ImGui::Separator();
            ImGui::TextUnformatted("C++ mod and ImGui are loaded.");
            ImGui::Text("UE4SS source: %s", UE4SS_LIB_BUILD_GITSHA);
            ImGui::TextWrapped(
                "Next: dump the game in a loaded save, then verify player and inventory "
                "interfaces.");
        });
        RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor GUI registered.\n"));
    }
};
}  // namespace

/** @brief Construct the UE4SS-owned mod instance; report construction failure as null. */
extern "C" __declspec(dllexport) auto start_mod() noexcept -> RC::CppUserModBase* {
    try {
        return new PBZEditorMod{};
    } catch (...) {
        return nullptr;
    }
}

/** @brief Release the mod instance; the UE4SS base class owns the registered GUI tab. */
extern "C" __declspec(dllexport) auto uninstall_mod(RC::CppUserModBase* mod) -> void {
    delete mod;
}
