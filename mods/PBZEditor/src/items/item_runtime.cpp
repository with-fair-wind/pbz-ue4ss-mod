/** @file item_runtime.cpp
 *  @brief Exact reflected calls for ordinary inventory stacks; no fixed game offsets.
 */
#include "item_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UEnum.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Windows.h>
#undef GetObject

namespace pbz::items {
namespace {
using namespace RC::Unreal;

[[nodiscard]] auto utf8(const std::wstring_view value) -> std::string {
    if (value.empty() || value.size() > 8192) {
        return {};
    }
    const auto size = static_cast<int>(value.size());
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                                           nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, result.data(), count,
                            nullptr, nullptr) != count) {
        return {};
    }
    return result;
}

[[nodiscard]] auto live(UObject* object) -> bool {
    return object != nullptr && object->GetClassPrivate() != nullptr &&
           !object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject |
                                                          RF_ArchetypeObject | RF_BeginDestroyed |
                                                          RF_FinishDestroyed));
}

[[nodiscard]] auto class_named(UClass* type, const wchar_t* expected) -> bool {
    const std::wstring_view name{expected};
    const auto identity = std::wstring{(name.starts_with(L"PBZ") || name == L"EventPipelineManager")
                                           ? L"/Script/ProjectPBZ."
                                           : L"/Script/Engine."} +
                          expected;
    for (auto* current = type; current != nullptr; current = current->GetSuperClass()) {
        if (current->GetPathName() == identity) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto property(UStruct* structure, const wchar_t* name) -> FProperty* {
    auto* member =
        structure == nullptr ? nullptr : structure->FindProperty(FName{name, FNAME_Find});
    return member != nullptr && member->GetArrayDim() == 1 ? member : nullptr;
}

template <typename T>
[[nodiscard]] auto field(UObject* object, const wchar_t* name) -> T* {
    auto* member = live(object) ? CastField<T>(object->GetPropertyByNameInChain(name)) : nullptr;
    return member != nullptr && member->GetArrayDim() == 1 ? member : nullptr;
}

[[nodiscard]] auto object_field(UObject* object, const wchar_t* name, const wchar_t* expected)
    -> UObject* {
    auto* member = field<FObjectPropertyBase>(object, name);
    if (member == nullptr || member->GetArrayDim() != 1 ||
        !class_named(member->GetPropertyClass().Get(), expected)) {
        return nullptr;
    }
    auto* result = member->GetObjectPropertyValue(member->ContainerPtrToValuePtr<void>(object));
    return live(result) && class_named(result->GetClassPrivate(), expected) ? result : nullptr;
}

/** Parameters use reflected size/alignment and engine initialization, including owned arrays. */
class FunctionParams {
public:
    explicit FunctionParams(UFunction* function) : function_(function) {
        const auto bytes =
            std::max<std::int32_t>(function->GetParmsSize(), function->GetPropertiesSize());
        if (bytes <= 0 || bytes > 1024 * 1024 ||
            function->GetMinAlignment() > alignof(std::max_align_t)) {
            throw std::runtime_error("Unsupported reflected parameter storage.");
        }
        storage_.resize((static_cast<std::size_t>(bytes) + sizeof(std::max_align_t) - 1) /
                        sizeof(std::max_align_t));
        function_->InitializeStruct(data());
    }
    FunctionParams(const FunctionParams&) = delete;
    auto operator=(const FunctionParams&) -> FunctionParams& = delete;
    ~FunctionParams() noexcept {
        try {
            function_->DestroyStruct(data());
        } catch (...) {
            try {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("PBZEditor: parameter destruction failed.\n"));
            } catch (...) {
                // The output sink may already be unavailable during process shutdown.
            }
        }
    }
    [[nodiscard]] auto data() noexcept -> void* {
        return storage_.data();
    }

private:
    UFunction* function_;
    std::vector<std::max_align_t> storage_;
};

[[nodiscard]] auto input(FProperty* member) -> bool {
    return member != nullptr && member->HasAnyPropertyFlags(CPF_Parm) &&
           !member->HasAnyPropertyFlags(CPF_ReturnParm) &&
           (!member->HasAnyPropertyFlags(CPF_OutParm) ||
            member->HasAnyPropertyFlags(CPF_ConstParm));
}

[[nodiscard]] auto output(FProperty* member) -> bool {
    return member != nullptr && member->HasAnyPropertyFlags(CPF_Parm | CPF_OutParm) &&
           member->HasAnyPropertyFlags(CPF_Parm) && member->HasAnyPropertyFlags(CPF_OutParm) &&
           !member->HasAnyPropertyFlags(CPF_ReturnParm | CPF_ConstParm);
}

[[nodiscard]] auto exact(UFunction* function, const std::size_t count) -> bool {
    if (function == nullptr || !function->HasAnyFunctionFlags(FUNC_Native)) {
        return false;
    }
    std::size_t found{};
    for (auto* member : TFieldRange<FProperty>(function, EFieldIterationFlags::IncludeDeprecated)) {
        if (!member->HasAnyPropertyFlags(CPF_Parm)) {
            continue;
        }
        if (member->GetArrayDim() != 1 || member->GetOffset_Internal() < 0 ||
            member->GetElementSize() <= 0 ||
            member->GetOffset_Internal() + member->GetElementSize() > function->GetParmsSize()) {
            return false;
        }
        ++found;
    }
    return found == count;
}

[[noreturn]] auto signature_error(UFunction* function, const char* message) -> void {
    if (function == nullptr) {
        RC::Output::send<RC::LogLevel::Warning>(
            STR("PBZEditor items signature: function not found.\n"));
    } else {
        RC::Output::send<RC::LogLevel::Warning>(
            STR("PBZEditor items signature: {} parameters={} bytes={} native={}\n"),
            function->GetPathName(), function->GetNumParms(), function->GetParmsSize(),
            function->HasAnyFunctionFlags(FUNC_Native));
        for (auto* member :
             TFieldRange<FProperty>(function, EFieldIterationFlags::IncludeDeprecated)) {
            if (member->HasAnyPropertyFlags(CPF_Parm)) {
                RC::Output::send<RC::LogLevel::Warning>(
                    STR("PBZEditor items signature: {} flags={} size={}\n"), member->GetFullName(),
                    static_cast<std::uint64_t>(member->GetPropertyFlags()),
                    member->GetElementSize());
            }
        }
    }
    throw std::runtime_error(message);
}

struct Context {
    UObject* world{};
    UObject* backpack{};
    UObject* game_instance{};
};

[[nodiscard]] auto context(UEngine* engine) -> Context {
    auto* viewport = object_field(engine, STR("GameViewport"), STR("GameViewportClient"));
    auto* world = object_field(viewport, STR("World"), STR("World"));
    auto* game_instance = object_field(viewport, STR("GameInstance"), STR("GameInstance"));
    auto* locals = field<FArrayProperty>(game_instance, STR("LocalPlayers"));
    auto* inner = locals == nullptr ? nullptr : CastField<FObjectPropertyBase>(locals->GetInner());
    if (!live(world) || inner == nullptr || locals->GetArrayFlags() != EArrayPropertyFlags::None ||
        !class_named(inner->GetPropertyClass().Get(), STR("LocalPlayer"))) {
        throw std::runtime_error("当前存档未就绪：无法确认本地玩家。");
    }
    FScriptArrayHelper_InContainer players{locals, game_instance};
    if (players.Num() != 1) {
        throw std::runtime_error("仅支持一个本地玩家的存档。");
    }
    auto* player = inner->GetObjectPropertyValue(players.GetRawPtr(0));
    auto* controller = object_field(player, STR("PlayerController"), STR("PlayerController"));
    auto* pawn = object_field(controller, STR("Pawn"), STR("Pawn"));
    if (!live(pawn) || !class_named(pawn->GetClassPrivate(), STR("PBZCharacter_Player"))) {
        throw std::runtime_error("请进入可操作角色的存档后再读取背包。");
    }
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(STR("PBZBackPackSubsystem"), candidates);
    UObject* selected{};
    for (auto* candidate : candidates) {
        if (!live(candidate) || candidate->GetOuterPrivate() != world ||
            !class_named(candidate->GetClassPrivate(), STR("PBZBackPackSubsystem"))) {
            continue;
        }
        if (selected != nullptr) {
            throw std::runtime_error("当前世界存在多个背包，拒绝选择不明确的目标。");
        }
        selected = candidate;
    }
    if (selected == nullptr) {
        throw std::runtime_error("当前世界背包尚未就绪。");
    }
    return {world, selected, game_instance};
}

/** Read the verified inventory map; do not invoke functions with unknown acquisition semantics. */
[[nodiscard]] auto all_items(const Context& ctx) -> std::vector<std::wstring> {
    auto* member = field<FMapProperty>(ctx.backpack, STR("Inventories"));
    auto* key = member == nullptr ? nullptr : CastField<FNameProperty>(member->GetKeyProp());
    auto* value =
        member == nullptr ? nullptr : CastField<FObjectPropertyBase>(member->GetValueProp());
    if (member == nullptr || key == nullptr || value == nullptr || key->GetArrayDim() != 1 ||
        value->GetArrayDim() != 1 || member->GetMapFlags() != EMapPropertyFlags::None ||
        !class_named(value->GetPropertyClass().Get(), STR("PBZInventoryInfo")) ||
        key->GetSize() <= 0 || value->GetSize() <= 0 || key->GetMinAlignment() <= 0 ||
        value->GetMinAlignment() <= 0) {
        throw std::runtime_error("Inventories 映射的键、值或分配器不兼容，拒绝读取。");
    }
    // UE4SS's own map reader builds this layout from the runtime property sizes/alignment.
    const auto layout = FScriptMap::GetScriptLayout(key->GetSize(), key->GetMinAlignment(),
                                                    value->GetSize(), value->GetMinAlignment());
    auto* values = member->ContainerPtrToValuePtr<FScriptMap>(ctx.backpack);
    const auto count = values->Num();
    const auto maximum = values->GetMaxIndex();
    if (count < 0 || maximum < count || static_cast<std::size_t>(count) > kMaximumItems ||
        static_cast<std::size_t>(maximum) > kMaximumItems * 4) {
        throw std::runtime_error("背包映射条目或稀疏索引数量超出支持范围。");
    }
    std::vector<std::wstring> paths;
    paths.reserve(static_cast<std::size_t>(count));
    for (int index{}; index < maximum; ++index) {
        if (!values->IsValidIndex(index)) {
            continue;
        }
        auto* entry = values->GetData(index, layout);
        const auto id = key->GetPropertyValue(entry);
        auto* object =
            value->GetObjectPropertyValue(static_cast<std::byte*>(entry) + layout.ValueOffset);
        auto* item_id = field<FNameProperty>(object, STR("ItemId"));
        if (!live(object) || !class_named(object->GetClassPrivate(), STR("PBZInventoryInfo")) ||
            id.IsNone() || item_id == nullptr ||
            item_id->GetPropertyValueInContainer(object) != id) {
            throw std::runtime_error("背包映射的物品身份不一致，拒绝读取。");
        }
        paths.push_back(object->GetPathName());
    }
    if (paths.size() != static_cast<std::size_t>(count)) {
        throw std::runtime_error("背包映射有效条目数量不一致。");
    }
    return paths;
}

[[nodiscard]] auto inventory_object(const std::wstring& path) -> UObject* {
    auto* object = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
    if (!live(object) || !class_named(object->GetClassPrivate(), STR("PBZInventoryInfo"))) {
        throw std::runtime_error("所选物品已失效，请重新读取背包。");
    }
    return object;
}

[[nodiscard]] auto integer(UObject* object, const wchar_t* name) -> std::int32_t {
    auto* member = field<FIntProperty>(object, name);
    if (member == nullptr || member->GetArrayDim() != 1) {
        throw std::runtime_error("物品整数属性不匹配。");
    }
    return member->GetPropertyValueInContainer(object);
}

[[nodiscard]] auto enum_byte(UStruct* structure, void* container, const wchar_t* name,
                             const wchar_t* expected) -> std::uint8_t {
    auto* member = property(structure, name);
    auto* enumeration = CastField<FEnumProperty>(member);
    if (enumeration != nullptr) {
        auto* underlying = CastField<FByteProperty>(enumeration->GetUnderlyingProperty());
        auto* type = enumeration->GetEnum().Get();
        if (underlying != nullptr && type != nullptr && type->GetName() == expected &&
            enumeration->GetArrayDim() == 1 && enumeration->GetElementSize() == 1) {
            return underlying->GetPropertyValue(
                enumeration->ContainerPtrToValuePtr<void>(container));
        }
    }
    auto* byte = CastField<FByteProperty>(member);
    if (byte != nullptr && byte->GetArrayDim() == 1 && byte->GetEnum().Get() != nullptr &&
        byte->GetEnum().Get()->GetName() == expected) {
        return byte->GetPropertyValueInContainer(container);
    }
    throw std::runtime_error("物品分类的枚举身份或宽度不匹配。");
}

[[nodiscard]] auto native_count(UObject* backpack, const FName id) -> std::int32_t {
    auto* function = backpack->GetFunctionByNameInChain(STR("GetItemNumInfo"));
    auto* input_id = CastField<FNameProperty>(property(function, STR("PropID")));
    auto* count = CastField<FIntProperty>(property(function, STR("CurrentNum")));
    auto* maximum = CastField<FIntProperty>(property(function, STR("MaxNum")));
    if (!exact(function, 3) || !input(input_id) || !output(count) || !output(maximum)) {
        signature_error(function, "GetItemNumInfo 签名不匹配，拒绝读写数量。");
    }
    FunctionParams params{function};
    input_id->SetPropertyValueInContainer(params.data(), id);
    backpack->ProcessEvent(function, params.data());
    return count->GetPropertyValueInContainer(params.data());
}

/** Optional native Flow localization; caller rechecks world after this final reflected call. */
[[nodiscard]] auto localized_label(const Context& ctx, FStructProperty* source, void* container)
    -> std::string {
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(STR("EventPipelineManager"), candidates);
    UObject* converter{};
    for (auto* candidate : candidates) {
        if (!live(candidate) || candidate->GetOuterPrivate() != ctx.world ||
            !class_named(candidate->GetClassPrivate(), STR("EventPipelineManager"))) {
            continue;
        }
        if (converter != nullptr) {
            return {};
        }
        converter = candidate;
    }
    if (converter == nullptr) {
        return {};
    }
    auto* function = converter->GetFunctionByNameInChain(STR("Conv_LocTextToString"));
    auto* argument = CastField<FStructProperty>(property(function, STR("InLocText")));
    auto* result =
        function == nullptr ? nullptr : CastField<FStrProperty>(function->GetReturnProperty());
    if (!exact(function, 2) || !input(argument) || result == nullptr ||
        !result->HasAnyPropertyFlags(CPF_ReturnParm) || result->GetArrayDim() != 1 ||
        argument->GetStruct().Get() != source->GetStruct().Get() ||
        argument->GetElementSize() != source->GetElementSize() ||
        result->GetElementSize() != sizeof(FString)) {
        return {};
    }
    FunctionParams params{function};
    argument->CopyCompleteValue(argument->ContainerPtrToValuePtr<void>(params.data()),
                                source->ContainerPtrToValuePtr<void>(container));
    converter->ProcessEvent(function, params.data());
    const auto& text = result->GetPropertyValueInContainer(params.data());
    if (text.Len() <= 0 || text.Len() > 8192) {
        return {};
    }
    return utf8(std::wstring_view{*text, static_cast<std::size_t>(text.Len())});
}

[[nodiscard]] auto read_item(UEngine* engine, const std::wstring& path, const std::wstring& world)
    -> Item {
    const auto ctx = context(engine);
    if (ctx.world->GetPathName() != world) {
        throw std::runtime_error("世界已改变，读取已取消。");
    }
    auto* object = inventory_object(path);
    auto* name = field<FNameProperty>(object, STR("ItemId"));
    if (name == nullptr) {
        throw std::runtime_error("物品 ID 属性不匹配。");
    }
    const auto id = name->GetPropertyValueInContainer(object);
    const auto backpack_path = ctx.backpack->GetPathName();
    const auto authoritative = native_count(ctx.backpack, id);
    object = inventory_object(path);  // ProcessEvent invalidates borrowed field/object assumptions.
    const auto current = context(engine);
    if (current.world->GetPathName() != world || current.backpack->GetPathName() != backpack_path) {
        throw std::runtime_error("背包所属世界已改变。");
    }
    name = field<FNameProperty>(object, STR("ItemId"));
    if (name == nullptr || name->GetPropertyValueInContainer(object) != id || id.IsNone()) {
        throw std::runtime_error("所选条目身份已改变。");
    }
    Item item;
    item.id = utf8(id.ToString());
    item.name = item.id;
    item.object_path = path;
    item.count = integer(object, STR("CurrentAmount"));
    item.historical_max = integer(object, STR("HasMaxAmount"));
    if (item.count != authoritative || item.count < 0 || item.historical_max < 0) {
        throw std::runtime_error("原生数量与物品条目不一致，请稍后刷新。");
    }
    auto* adds = field<FBoolProperty>(object, STR("bIsAddAttributeItem"));
    auto* data = field<FStructProperty>(object, STR("ItemDataStruct"));
    if (adds == nullptr || data == nullptr || data->GetStruct().Get() == nullptr ||
        data->GetStruct().Get()->GetName() != STR("PBZItemDataStruct")) {
        throw std::runtime_error("物品数据结构不匹配。");
    }
    item.adds_attribute = adds->GetPropertyValueInContainer(object);
    auto* structure = data->GetStruct().Get();
    auto* storage = data->ContainerPtrToValuePtr<void>(object);
    item.type = enum_byte(structure, storage, STR("ItemType"), STR("EItemTypes"));
    item.detail = enum_byte(structure, storage, STR("ItemDetailType"), STR("EItemDetailType"));
    item.material_type =
        enum_byte(structure, storage, STR("MaterialType"), STR("EItemMaterialType"));
    auto* integration = CastField<FIntProperty>(property(structure, STR("IntegrationItemNum")));
    auto* target = CastField<FNameProperty>(property(structure, STR("IntegrationItemID")));
    if (integration == nullptr || target == nullptr) {
        throw std::runtime_error("无法验证物品自动合成状态。");
    }
    item.integration_count = integration->GetPropertyValueInContainer(storage);
    item.integration_target = !target->GetPropertyValueInContainer(storage).IsNone();
    auto* label = CastField<FStructProperty>(property(structure, STR("ItemName")));
    if (label != nullptr && label->GetStruct().Get() != nullptr &&
        label->GetStruct().Get()->GetName() == STR("FlowLocText")) {
        auto* text =
            CastField<FTextProperty>(property(label->GetStruct().Get(), STR("ValueCache")));
        if (text != nullptr && text->GetElementSize() == FText::StaticSize() &&
            FText::StaticSize() > 0 && FText::StaticSize() <= sizeof(FText) &&
            FText::ValueOpsProperty != nullptr) {
            const auto* source =
                text->ContainerPtrToValuePtr<FText>(label->ContainerPtrToValuePtr<void>(storage));
            if (source->GetTextData() != nullptr) {
                FText owned;
                // The engine copies its runtime-sized text; generic TProperty expects
                // sizeof(FText).
                text->CopyCompleteValue(&owned, source);
                const auto display = utf8(owned.ToString());
                if (!display.empty()) {
                    item.name = display;
                }
            }
        }
    }
    if (item.name == item.id && label != nullptr && label->GetStruct().Get() != nullptr &&
        label->GetStruct().Get()->GetName() == STR("FlowLocText")) {
        const auto display = localized_label(current, label, storage);
        const auto after_label = context(engine);
        if (after_label.world->GetPathName() != world ||
            after_label.backpack->GetPathName() != backpack_path) {
            throw std::runtime_error("读取名称期间世界已改变。");
        }
        if (!display.empty()) {
            item.name = display;
        }
    }
    return item;
}

/** Validate both native write directions before starting any write or rollback. */
auto validate_delta(UObject* backpack, const bool increase) -> void {
    auto* function = backpack->GetFunctionByNameInChain(increase ? STR("AddPropsToBackPack")
                                                                 : STR("RemovePropsFromBackPack"));
    auto* amount = CastField<FIntProperty>(property(function, STR("Num")));
    auto* id = CastField<FNameProperty>(property(function, STR("ItemId")));
    auto* notify = CastField<FBoolProperty>(
        property(function, increase ? STR("bPushWidget") : STR("bSendBroadcast")));
    if (!exact(function, 3) || !input(amount) || !input(id) || !input(notify)) {
        signature_error(function, "原生物品增减函数签名不匹配。");
    }
}

auto native_delta(UObject* backpack, const FName id, const std::int32_t delta) -> void {
    const bool increase = delta > 0;
    validate_delta(backpack, increase);
    auto* function = backpack->GetFunctionByNameInChain(increase ? STR("AddPropsToBackPack")
                                                                 : STR("RemovePropsFromBackPack"));
    FunctionParams params{function};
    CastField<FIntProperty>(property(function, STR("Num")))
        ->SetPropertyValueInContainer(params.data(), increase ? delta : -delta);
    CastField<FNameProperty>(property(function, STR("ItemId")))
        ->SetPropertyValueInContainer(params.data(), id);
    CastField<FBoolProperty>(
        property(function, increase ? STR("bPushWidget") : STR("bSendBroadcast")))
        ->SetPropertyValueInContainer(params.data(), !increase);
    backpack->ProcessEvent(function, params.data());
}
UObject* find_object(const std::wstring& path) {
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
}

UObject* static_receiver(const std::wstring& owner) {
    const auto split = owner.rfind(L'.');
    auto* result = find_object(owner.substr(0, split + 1) + L"Default__" + owner.substr(split + 1));
    if (!result || !result->HasAnyFlags(RF_ClassDefaultObject) ||
        result->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)) ||
        !result->GetClassPrivate() || result->GetClassPrivate()->GetPathName() != owner) {
        throw std::runtime_error("Probe static receiver unavailable");
    }
    return result;
}

struct CatalogSource {
    const wchar_t* path;
    const wchar_t* schema;
    int size;
    Category category;
    const wchar_t* name;
    const wchar_t* description;
};
constexpr std::array kCatalogSources{
    CatalogSource{L"/Game/PBZContent/DataTable/Item/DT_InventoryData.DT_InventoryData",
                  L"PBZItemDataStruct", 856, Category::items, L"ItemName", L"ItemDescription"},
    CatalogSource{L"/Game/PBZContent/DataTable/UI/DT_WeaponData.DT_WeaponData", L"WeaponData", 1880,
                  Category::weapon, L"WeaponName", L"EquipDescription"},
    CatalogSource{L"/Game/PBZContent/DataTable/UI/DT_ShaqiWeaponData.DT_ShaqiWeaponData",
                  L"ShaqiWeaponData", 2096, Category::shaqi, L"WeaponName", L"EquipDescription"},
    CatalogSource{L"/Game/PBZContent/DataTable/UI/DT_MedicineTable.DT_MedicineTable",
                  L"PBZMedicineDataStruct", 688, Category::medicine, L"Name", L"Desc"},
    CatalogSource{L"/Game/PBZContent/DataTable/UI/DT_Accessory.DT_Accessory", L"AccessoryData", 536,
                  Category::accessory, L"AccessoryName", L"AccessoryDesc"},
    CatalogSource{L"/Game/PBZContent/DataTable/Item/DT_BookData.DT_BookData", L"BookDataStruct",
                  360, Category::book, L"BookName", L"BookDesc"},
    // The NPC codex source is audited but has no inventory category.
    CatalogSource{L"/Game/PBZContent/DataTable/Item/DT_CodexData.DT_CodexData", L"CodexDataStruct",
                  168, Category::book, nullptr, nullptr},
};

auto table_object(const CatalogSource& source) -> const UDataTable* {
    auto* table = find_object(source.path);
    if (!live(table) || !class_named(table->GetClassPrivate(), L"DataTable")) {
        throw std::runtime_error("Catalog table is not available in the current world");
    }
    return static_cast<const UDataTable*>(table);
}

auto checked_schema(const UDataTable* table, const CatalogSource& source) -> UScriptStruct* {
    auto* schema = table->GetRowStruct().Get();
    if (!live(schema) ||
        schema->GetPathName() != std::wstring(L"/Script/ProjectPBZ.") + source.schema ||
        schema->GetPropertiesSize() != source.size) {
        throw std::runtime_error("Catalog table schema differs from verified schema");
    }
    return schema;
}

auto validate_table(const CatalogSource& source, bool read_names) -> std::vector<std::wstring> {
    const auto* table = table_object(source);
    auto* library = static_receiver(L"/Script/Engine.DataTableFunctionLibrary");
    const auto* method = read_names ? L"GetDataTableRowNames" : L"GetDataTableRowStruct";
    auto* function = library->GetFunctionByNameInChain(method);
    auto* target = CastField<FObjectProperty>(property(function, L"Table"));
    if (!exact(function, 2) || !function->HasAnyFunctionFlags(FUNC_Static) ||
        function->GetPathName() !=
            std::wstring(L"/Script/Engine.DataTableFunctionLibrary:") + method ||
        !input(target) || target->HasAnyPropertyFlags(CPF_TObjectPtr) ||
        target->GetElementSize() != sizeof(UObject*) || !target->GetPropertyClass().Get() ||
        target->GetPropertyClass().Get()->GetPathName() != L"/Script/Engine.DataTable") {
        throw std::runtime_error("Unsupported DataTableFunctionLibrary signature");
    }
    FunctionParams params{function};
    target->SetPropertyValueInContainer(params.data(), const_cast<UDataTable*>(table));
    if (read_names) {
        auto* array = CastField<FArrayProperty>(property(function, L"OutRowNames"));
        auto* name = array ? CastField<FNameProperty>(array->GetInner()) : nullptr;
        if (!output(array) || array->GetArrayFlags() != EArrayPropertyFlags::None ||
            array->GetElementSize() != sizeof(FScriptArray) || !name || name->GetArrayDim() != 1 ||
            name->GetElementSize() != sizeof(FName)) {
            throw std::runtime_error("Unsupported native table names");
        }
        library->ProcessEvent(function, params.data());
        FScriptArrayHelper_InContainer rows{array, params.data()};
        const auto count = rows.Num();
        if (count < 0 || count > 50000) {
            throw std::runtime_error("Native table names exceed bound");
        }
        const auto* current = table_object(source);
        if (current != table) {
            throw std::runtime_error("DataTable changed during native row query");
        }
        static_cast<void>(checked_schema(current, source));
        const auto& map = current->GetRowMap();
        if (map.Num() != count) {
            throw std::runtime_error("UE4SS row map count differs from native row names");
        }
        // Cross-check identities through enumeration, using map order only for the local set.
        std::vector<std::wstring> map_names;
        std::vector<std::wstring> native_names;
        map_names.reserve(count);
        native_names.reserve(count);
        for (const auto& pair : map) {
            if (pair.Key.IsNone() || !pair.Value ||
                map_names.size() >= static_cast<std::size_t>(count)) {
                throw std::runtime_error("Invalid UE4SS row map entry");
            }
            map_names.push_back(pair.Key.ToString());
        }
        for (int index = 0; index < count; ++index) {
            native_names.push_back(name->GetPropertyValue(rows.GetRawPtr(index)).ToString());
        }
        std::ranges::sort(map_names);
        std::ranges::sort(native_names);
        if (map_names != native_names) {
            throw std::runtime_error("UE4SS row keys differ from native row names");
        }
        if (std::ranges::adjacent_find(native_names) != native_names.end()) {
            throw std::runtime_error("Duplicate catalog row key");
        }
        return native_names;
    }
    auto* result = CastField<FObjectProperty>(property(function, L"ReturnValue"));
    if (!result || !result->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm) ||
        result->HasAnyPropertyFlags(CPF_TObjectPtr) ||
        result->GetElementSize() != sizeof(UObject*) || !result->GetPropertyClass().Get() ||
        result->GetPropertyClass().Get()->GetPathName() != L"/Script/CoreUObject.ScriptStruct") {
        throw std::runtime_error("Unsupported native table row type");
    }
    library->ProcessEvent(function, params.data());
    auto* row = result->GetPropertyValueInContainer(params.data());
    if (!live(row) ||
        row->GetClassPrivate()->GetPathName() != L"/Script/CoreUObject.ScriptStruct") {
        throw std::runtime_error("Missing native script struct");
    }
    const auto* current = table_object(source);
    if (current != table || checked_schema(current, source) != row) {
        throw std::runtime_error("UE4SS row schema differs from native result");
    }
    return {};
}

auto within_row(FProperty* member, const UStruct* schema) -> bool {
    return member && member->GetArrayDim() == 1 && member->GetElementSize() > 0 &&
           member->GetOffset_Internal() >= 0 &&
           member->GetOffset_Internal() <= schema->GetPropertiesSize() - member->GetElementSize();
}

// Acquisition can create a new object and a historical record, so the ordinary stack
// transaction's native removal does not restore the original absent state.
auto ordinary_material(UScriptStruct* schema, void* row) -> bool {
    if (!within_row(property(schema, L"ItemType"), schema) ||
        !within_row(property(schema, L"ItemDetailType"), schema) ||
        !within_row(property(schema, L"MaterialType"), schema) ||
        enum_byte(schema, row, L"ItemType", L"EItemTypes") != 0 ||
        enum_byte(schema, row, L"ItemDetailType", L"EItemDetailType") != 1) {
        return false;
    }
    const auto material = enum_byte(schema, row, L"MaterialType", L"EItemMaterialType");
    auto* flow = CastField<FSoftObjectProperty>(property(schema, L"RunFlowAsset"));
    auto* attribute = CastField<FSoftClassProperty>(property(schema, L"AddAttributeGE"));
    auto* integration = CastField<FIntProperty>(property(schema, L"IntegrationItemNum"));
    auto* target = CastField<FNameProperty>(property(schema, L"IntegrationItemID"));
    auto* flower = CastField<FBoolProperty>(property(schema, L"bIsWhiteFlower"));
    auto* reward = CastField<FNameProperty>(property(schema, L"DuplicatePetalDustLootID"));
    auto* ability = CastField<FSoftClassProperty>(property(schema, L"ItemGA"));
    auto* dialogue = CastField<FArrayProperty>(property(schema, L"DialogueGroupIDs"));
    auto* dialogue_id = dialogue ? CastField<FNameProperty>(dialogue->GetInner()) : nullptr;
    if (!within_row(dialogue, schema) || dialogue->GetArrayFlags() != EArrayPropertyFlags::None ||
        dialogue->GetElementSize() != sizeof(FScriptArray) || !dialogue_id ||
        dialogue_id->GetArrayDim() != 1 || dialogue_id->GetElementSize() != sizeof(FName) ||
        FScriptArrayHelper_InContainer{dialogue, row}.Num() != 0) {
        return false;
    }
    const auto soft_size = FSoftObjectPtr::StaticSize();
    const FSoftObjectPtr null_reference;
    if (material < 1 || material > 6 || soft_size <= 0 ||
        static_cast<std::size_t>(soft_size) > sizeof(null_reference) || !within_row(flow, schema) ||
        flow->GetElementSize() != soft_size || !within_row(ability, schema) ||
        ability->GetElementSize() != soft_size ||
        !ability->Identical(ability->ContainerPtrToValuePtr<void>(row), &null_reference) ||
        !within_row(attribute, schema) || attribute->GetElementSize() != soft_size ||
        !within_row(integration, schema) || integration->GetElementSize() != sizeof(std::int32_t) ||
        !within_row(target, schema) || target->GetElementSize() != sizeof(FName) ||
        !within_row(flower, schema) || !within_row(reward, schema) ||
        reward->GetElementSize() != sizeof(FName) ||
        integration->GetPropertyValueInContainer(row) != 0 ||
        !target->GetPropertyValueInContainer(row).IsNone() ||
        flower->GetPropertyValueInContainer(row) ||
        !reward->GetPropertyValueInContainer(row).IsNone() ||
        !flow->Identical(flow->ContainerPtrToValuePtr<void>(row), &null_reference) ||
        !attribute->Identical(attribute->ContainerPtrToValuePtr<void>(row), &null_reference)) {
        return false;
    }
    return true;
}

auto grant_identity(const CatalogItem& item) -> FName {
    const auto source_it = std::ranges::find_if(kCatalogSources, [&](const CatalogSource& entry) {
        return entry.name && utf8(entry.path) == item.source &&
               (entry.category == item.category ||
                (entry.category == Category::items && item.category == Category::book));
    });
    if (source_it == kCatalogSources.end() || !eligible_for_grant(item)) {
        throw std::runtime_error("所选条目没有可用的获得途径。");
    }
    const auto& source = *source_it;
    const auto* table = table_object(source);
    auto* schema = checked_schema(table, source);
    auto* id = CastField<FNameProperty>(property(schema, L"ItemId"));
    const auto row_name = RC::ensure_str(item.row_key);
    const FName key{row_name.c_str(), FNAME_Find};
    const auto& rows = table->GetRowMap();
    const auto* row = key.IsNone() ? nullptr : rows.Find(key);
    if (!row || !*row || !within_row(id, schema) || id->GetElementSize() != sizeof(FName) ||
        rows.Num() <= 0 || rows.Num() > 1024) {
        throw std::runtime_error("无法确认物品目录中的物品条目。");
    }
    const auto selected = id->GetPropertyValueInContainer(*row);
    if (selected.IsNone() || utf8(selected.ToString()) != item.id) {
        throw std::runtime_error("物品目录中的物品身份已变化。");
    }
    std::size_t matches{};
    for (const auto& entry : rows) {
        if (entry.Value && id->GetPropertyValueInContainer(entry.Value) == selected) {
            ++matches;
        }
    }
    if (matches != 1 || (item.category == Category::items && !ordinary_material(schema, *row)) ||
        (item.category == Category::book &&
         (source.category != Category::items ||
          !within_row(property(schema, L"ItemType"), schema) ||
          enum_byte(schema, *row, L"ItemType", L"EItemTypes") != 1))) {
        throw std::runtime_error("物品规则已变化或 ID 重复，请刷新目录。");
    }
    return selected;
}

auto grant_context(UEngine* engine, const Request& request) -> Context {
    const auto ctx = context(engine);
    if (ctx.world->GetPathName() != request.world ||
        ctx.backpack->GetPathName() != request.backpack) {
        throw std::runtime_error("目录对应的世界或背包已变化，请重新读取。");
    }
    return ctx;
}

struct GrantInventory {
    std::int32_t count{};
    std::optional<std::int32_t> historical_max;
};

auto grant_inventory(UEngine* engine, const Request& request, const FName id) -> GrantInventory {
    const auto count = native_count(grant_context(engine, request).backpack, id);
    const auto ctx = grant_context(engine, request);
    std::optional<std::int32_t> historical_max;
    for (const auto& path : all_items(ctx)) {
        auto* object = inventory_object(path);
        auto* name = field<FNameProperty>(object, L"ItemId");
        if (!name) {
            throw std::runtime_error("无法确认背包中的物品身份。");
        }
        if (name->GetPropertyValueInContainer(object) != id) {
            continue;
        }
        if (historical_max || integer(object, L"CurrentAmount") != count) {
            throw std::runtime_error("背包条目重复或原生数量与条目不一致。");
        }
        historical_max = integer(object, L"HasMaxAmount");
    }
    if (count < 0 || (historical_max ? *historical_max < count : count != 0)) {
        throw std::runtime_error("无法确认材料数量。");
    }
    return {count, historical_max};
}

auto set_save_enum(UStruct* schema, const wchar_t* name, const wchar_t* enum_path,
                   const std::uint8_t value, const std::wstring_view label, void* storage) -> void {
    auto* member = property(schema, name);
    auto* enumeration = CastField<FEnumProperty>(member);
    auto* byte = enumeration ? CastField<FByteProperty>(enumeration->GetUnderlyingProperty())
                             : CastField<FByteProperty>(member);
    auto* type =
        enumeration ? enumeration->GetEnum().Get() : (byte ? byte->GetEnum().Get() : nullptr);
    if (!within_row(member, schema) || member->GetElementSize() != 1 || !byte ||
        byte->GetElementSize() != 1 || !type || type->GetPathName() != enum_path ||
        !type->GetNameByValue(value).ToString().ends_with(label)) {
        throw std::runtime_error("原生保存的枚举字段不兼容。");
    }
    byte->SetPropertyValue(member->ContainerPtrToValuePtr<void>(storage), value);
}

auto save_owner(const Context& ctx) -> UObject* {
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(L"PBZSaveLoadSubsystem", candidates);
    UObject* owner{};
    for (auto* candidate : candidates) {
        if (!live(candidate) || candidate->GetOuterPrivate() != ctx.game_instance ||
            !class_named(candidate->GetClassPrivate(), L"PBZSaveLoadSubsystem")) {
            continue;
        }
        if (owner) {
            throw std::runtime_error("当前游戏存在多个保存子系统。");
        }
        owner = candidate;
    }
    if (!owner) {
        throw std::runtime_error("当前游戏的保存子系统尚未就绪。");
    }
    return owner;
}

auto continue_slot(UObject* save_owner) -> std::wstring {
    auto* function = save_owner->GetFunctionByNameInChain(L"GetContinueSlotName");
    auto* result = CastField<FStrProperty>(property(function, L"ReturnValue"));
    if (!exact(function, 1) || !result || !result->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm) ||
        result->GetElementSize() != sizeof(FString) ||
        function->GetPathName() != L"/Script/ProjectPBZ.PBZSaveLoadSubsystem:GetContinueSlotName" ||
        function->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate)) {
        signature_error(function, "游戏继续槽位的读取接口不兼容。");
    }
    FunctionParams params{function};
    save_owner->ProcessEvent(function, params.data());
    const auto& value = result->GetPropertyValueInContainer(params.data());
    if (value.Len() <= 0 || value.Len() > 96) {
        throw std::runtime_error("游戏当前没有可确认的继续槽位。");
    }
    return {*value, static_cast<std::size_t>(value.Len())};
}

// The Flow subsystem belongs to the active game instance and holds its loaded save object.
auto loaded_slot(const Context& ctx) -> std::wstring {
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(L"FlowSubsystem", candidates);
    UObject* loaded{};
    for (auto* candidate : candidates) {
        if (!live(candidate) || candidate->GetOuterPrivate() != ctx.game_instance) {
            continue;
        }
        auto* prop = field<FObjectPropertyBase>(candidate, L"LoadedSaveGame");
        if (!prop || !prop->GetPropertyClass().Get() ||
            prop->GetPropertyClass().Get()->GetPathName() != L"/Script/Flow.FlowSaveGame") {
            continue;
        }
        auto* current = prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<void>(candidate));
        if (!live(current)) {
            continue;
        }
        if (!current->IsA(prop->GetPropertyClass().Get()) || (loaded && current != loaded)) {
            return {};
        }
        loaded = current;
    }
    auto* name = field<FStrProperty>(loaded, L"SaveSlotName");
    if (!name || name->GetElementSize() != sizeof(FString)) {
        return {};
    }
    const auto& value = name->GetPropertyValueInContainer(loaded);
    if (value.Len() <= 0 || value.Len() > 96) {
        return {};
    }
    return {*value, static_cast<std::size_t>(value.Len())};
}

auto confirmed_slot(UEngine* engine, const Request& request) -> std::wstring {
    const auto selected = continue_slot(save_owner(grant_context(engine, request)));
    const auto loaded = loaded_slot(grant_context(engine, request));
    RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor slots: loaded={} continue={}\n"), loaded,
                                            selected);
    if (loaded.empty() || loaded != selected || selected.find(L'\0') != std::wstring::npos) {
        return {};
    }
    return selected;
}

enum class SaveDisposition { preflight, requested, suspended, refused };

auto prepare_or_request_save(const Context& ctx, const std::wstring& selected_slot,
                             const bool submit) -> SaveDisposition {
    auto* owner = save_owner(ctx);
    auto* function = owner->GetFunctionByNameInChain(L"TrySave");
    auto* reason = property(function, L"Reason");
    auto* callback =
        CastField<FDelegateProperty>(property(function, L"AsyncSaveGameToSlotDelegate"));
    auto* param = CastField<FStructProperty>(property(function, L"SaveGameParam"));
    auto* schema = param ? param->GetStruct().Get() : nullptr;
    auto* result = CastField<FEnumProperty>(property(function, L"ReturnValue"));
    auto* result_type = result ? result->GetEnum().Get() : nullptr;
    auto* result_byte =
        result ? CastField<FByteProperty>(result->GetUnderlyingProperty()) : nullptr;
    if (!exact(function, 4) || !result || !result_type || !result_byte ||
        result->GetElementSize() != 1 || result_byte->GetElementSize() != 1 ||
        !result->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm) ||
        result_type->GetPathName() != L"/Script/ProjectPBZ.EPBZTrySaveResult" ||
        !result_type->GetNameByValue(0).ToString().ends_with(L"::SavedNow") ||
        !result_type->GetNameByValue(1).ToString().ends_with(L"::Suspended") ||
        !result_type->GetNameByValue(2).ToString().ends_with(L"::RejectedShutdown") ||
        function->GetPathName() != L"/Script/ProjectPBZ.PBZSaveLoadSubsystem:TrySave" ||
        function->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate) || !input(param) ||
        !input(reason) || !input(callback) ||
        callback->GetElementSize() != sizeof(FScriptDelegate) || !schema ||
        schema->GetPathName() != L"/Script/ProjectPBZ.SaveGameParam" ||
        param->GetElementSize() != schema->GetPropertiesSize()) {
        signature_error(function, "游戏的原生保存接口不兼容。");
    }
    auto* slot = CastField<FStrProperty>(property(schema, L"SaveSlotName"));
    auto* transform =
        CastField<FStructProperty>(property(schema, L"OverridePlayerCharacterSaveTransform"));
    auto* transform_type = transform ? transform->GetStruct().Get() : nullptr;
    if (!within_row(slot, schema) || slot->GetElementSize() != sizeof(FString) ||
        !within_row(transform, schema) || !transform_type ||
        transform_type->GetPathName() != L"/Script/CoreUObject.Transform" ||
        transform->GetElementSize() != transform_type->GetPropertiesSize()) {
        throw std::runtime_error("游戏的槽位或角色位置字段不兼容。");
    }
    FunctionParams params{function};
    if (callback->GetPropertyValueInContainer(params.data()).IsBound()) {
        throw std::runtime_error("保存参数中的回调状态异常。");
    }
    set_save_enum(function, L"Reason", L"/Script/ProjectPBZ.EPBZSaveReason", 4, L"::ManualSave",
                  params.data());
    auto* storage = param->ContainerPtrToValuePtr<void>(params.data());
    for (const auto* name : {L"bOverridePlayerCharacterTransform", L"bIsRevive",
                             L"bIsOverrideLevelName", L"bSaveAdvanced", L"bShowSaveTip"}) {
        auto* flag = CastField<FBoolProperty>(property(schema, name));
        if (!within_row(flag, schema)) {
            throw std::runtime_error("游戏的原生保存标记字段不兼容。");
        }
        flag->SetPropertyValueInContainer(storage, std::wstring_view{name} == L"bShowSaveTip");
    }
    for (const auto* name : {L"NewLevelName", L"BellID", L"DisplayBellID"}) {
        auto* field = CastField<FNameProperty>(property(schema, name));
        if (!within_row(field, schema) || field->GetElementSize() != sizeof(FName)) {
            throw std::runtime_error("游戏的关卡或存档点字段不兼容。");
        }
        field->SetPropertyValueInContainer(storage, FName{});
    }
    set_save_enum(schema, L"IoContext", L"/Script/ProjectPBZ.EPBZSaveIOContext", 0, L"::Legacy",
                  storage);
    set_save_enum(schema, L"Reason", L"/Script/ProjectPBZ.EPBZSaveReason", 4, L"::ManualSave",
                  storage);
    slot->SetPropertyValueInContainer(storage, FString{selected_slot.c_str()});
    if (!submit) {
        return SaveDisposition::preflight;
    }
    owner->ProcessEvent(function, params.data());
    const auto code =
        result_byte->GetPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor grant save: slot={} disposition={}\n"),
                                            selected_slot, code);
    switch (code) {
        case 0:
            return SaveDisposition::requested;
        case 1:
            return SaveDisposition::suspended;
        case 2:
            return SaveDisposition::refused;
        default:
            throw std::runtime_error("游戏返回了未知的保存状态。");
    }
}

auto catalog_label(const Context& ctx, UScriptStruct* schema, void* row, const wchar_t* field_name)
    -> std::string {
    auto* text = CastField<FStructProperty>(property(schema, field_name));
    if (!within_row(text, schema) || !text->GetStruct().Get() ||
        text->GetStruct().Get()->GetPathName() != L"/Script/ProjectPBZ.FlowLocText" ||
        text->GetStruct().Get()->GetPropertiesSize() != text->GetElementSize()) {
        return {};
    }
    return localized_label(ctx, text, row);
}

auto catalog_text(const CatalogSource& source, const std::wstring& key, const Context& ctx,
                  const wchar_t* field_name, std::string_view expected_id) -> std::string {
    const auto* table = table_object(source);
    auto* schema = checked_schema(table, source);
    const FName row_name{key.c_str(), FNAME_Find};
    const auto* row = row_name.IsNone() ? nullptr : table->GetRowMap().Find(row_name);
    auto* id = CastField<FNameProperty>(property(schema, L"ItemId"));
    if (!row || !*row || !within_row(id, schema) || id->GetElementSize() != sizeof(FName) ||
        utf8(id->GetPropertyValueInContainer(*row).ToString()) != expected_id) {
        throw std::runtime_error("Catalog row changed during text lookup");
    }
    return catalog_label(ctx, schema, *row, field_name);
}

auto catalog_row(const CatalogSource& source, const std::wstring& key, const Context& ctx)
    -> std::optional<CatalogItem> {
    const auto* table = table_object(source);
    auto* schema = checked_schema(table, source);
    const FName row_key{key.c_str(), FNAME_Find};
    const auto* row = row_key.IsNone() ? nullptr : table->GetRowMap().Find(row_key);
    if (!row || !*row) {
        throw std::runtime_error("Catalog row disappeared");
    }
    auto* id_field = CastField<FNameProperty>(property(schema, L"ItemId"));
    if (!within_row(id_field, schema) || id_field->GetElementSize() != sizeof(FName)) {
        throw std::runtime_error("Catalog row has incompatible ItemId");
    }
    const auto id = id_field->GetPropertyValueInContainer(*row);
    if (id.IsNone() || !source.name) {
        return std::nullopt;
    }
    const auto id_text = id.ToString();
    if (source.category == Category::weapon &&
        (id_text == L"Tang_HorseCartCombat" || id_text == L"TangPrologue")) {
        return std::nullopt;
    }
    CatalogItem result;
    result.category = source.category;
    result.id = utf8(id_text);
    result.source = utf8(source.path);
    result.row_key = utf8(key);
    result.grantable_definition =
        source.category != Category::items && source.category != Category::book;
    if (source.category == Category::items) {
        try {
            if (within_row(property(schema, L"ItemType"), schema) &&
                enum_byte(schema, *row, L"ItemType", L"EItemTypes") == 1) {
                result.category = Category::book;
                result.grantable_definition = true;
            } else {
                result.grantable_definition = ordinary_material(schema, *row);
            }
        } catch (const std::exception&) {
            result.grantable_definition = false;
        }
    }
    result.name = catalog_label(ctx, schema, *row, source.name);
    if (result.name.empty()) {
        result.name = source.category == Category::book ? result.row_key : result.id;
    }
    return result;
}

auto owned_book_or_medicine_ids(const Context& ctx, Category category)
    -> std::optional<std::vector<std::string>> {
    const bool books = category == Category::book;
    auto* function =
        ctx.backpack->GetFunctionByNameInChain(books ? L"GetAllOwnedBookIDs" : L"GetAllMedicines");
    auto* array = CastField<FArrayProperty>(property(function, L"ReturnValue"));
    auto* name = array ? CastField<FNameProperty>(array->GetInner()) : nullptr;
    if (!exact(function, 1) || function->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate) ||
        function->GetPathName() !=
            (books ? L"/Script/ProjectPBZ.PBZBackPackSubsystem:GetAllOwnedBookIDs"
                   : L"/Script/ProjectPBZ.PBZBackPackSubsystem:GetAllMedicines") ||
        !array || !array->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm) ||
        array->GetArrayFlags() != EArrayPropertyFlags::None ||
        array->GetElementSize() != sizeof(FScriptArray) || !name || name->GetArrayDim() != 1 ||
        name->GetElementSize() != sizeof(FName)) {
        return std::nullopt;
    }
    FunctionParams params{function};
    ctx.backpack->ProcessEvent(function, params.data());
    FScriptArrayHelper_InContainer values{array, params.data()};
    const auto count = values.Num();
    if (count < 0 || count > 1024) {
        return std::nullopt;
    }
    std::vector<std::string> owned;
    owned.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto item = name->GetPropertyValue(values.GetRawPtr(index));
        if (item.IsNone()) {
            return std::nullopt;
        }
        owned.push_back(utf8(item.ToString()));
    }
    return owned;
}

auto owned_accessories(const Context& ctx) -> std::optional<std::vector<std::string>> {
    auto* function = ctx.backpack->GetFunctionByNameInChain(L"GetAccessoryInfos");
    auto* map = CastField<FMapProperty>(property(function, L"ReturnValue"));
    auto* key = map ? CastField<FNameProperty>(map->GetKeyProp()) : nullptr;
    auto* value = map ? CastField<FObjectPropertyBase>(map->GetValueProp()) : nullptr;
    if (!exact(function, 1) ||
        function->GetPathName() != L"/Script/ProjectPBZ.PBZBackPackSubsystem:GetAccessoryInfos" ||
        !map || !map->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm) ||
        map->GetMapFlags() != EMapPropertyFlags::None ||
        map->GetElementSize() != sizeof(FScriptMap) || !key || key->GetArrayDim() != 1 ||
        key->GetElementSize() != sizeof(FName) || key->GetMinAlignment() <= 0 || !value ||
        value->GetArrayDim() != 1 || value->GetElementSize() != sizeof(UObject*) ||
        value->GetMinAlignment() <= 0 ||
        !class_named(value->GetPropertyClass().Get(), L"PBZAccessoryInfo")) {
        return std::nullopt;
    }
    FunctionParams params{function};
    ctx.backpack->ProcessEvent(function, params.data());
    const auto layout = FScriptMap::GetScriptLayout(key->GetSize(), key->GetMinAlignment(),
                                                    value->GetSize(), value->GetMinAlignment());
    auto* values = map->ContainerPtrToValuePtr<FScriptMap>(params.data());
    const auto count = values->Num();
    const auto maximum = values->GetMaxIndex();
    if (count < 0 || maximum < count || maximum > 4096) {
        return std::nullopt;
    }
    std::vector<std::string> owned;
    owned.reserve(count);
    for (int index = 0; index < maximum; ++index) {
        if (!values->IsValidIndex(index)) {
            continue;
        }
        auto* entry = values->GetData(index, layout);
        const auto item = key->GetPropertyValue(entry);
        auto* object =
            value->GetObjectPropertyValue(static_cast<std::byte*>(entry) + layout.ValueOffset);
        if (item.IsNone() || !live(object) ||
            !class_named(object->GetClassPrivate(), L"PBZAccessoryInfo")) {
            return std::nullopt;
        }
        owned.push_back(utf8(item.ToString()));
    }
    if (owned.size() != static_cast<std::size_t>(count)) {
        return std::nullopt;
    }
    return owned;
}

auto catalog_ownership(const Context& ctx, const CatalogItem& item) -> Ownership {
    const auto name = RC::ensure_str(item.id);
    const FName id{name.c_str(), FNAME_Find};
    if (id.IsNone()) {
        return Ownership::unknown;
    }
    if (item.category == Category::items ||
        (item.category == Category::book && item.source == utf8(kCatalogSources.front().path))) {
        try {
            const auto count = native_count(ctx.backpack, id);
            return count > 0    ? Ownership::owned
                   : count == 0 ? Ownership::unowned
                                : Ownership::unknown;
        } catch (const std::exception&) {
            return Ownership::unknown;
        }
    }
    if (item.category == Category::weapon || item.category == Category::shaqi) {
        auto* function = ctx.backpack->GetFunctionByNameInChain(L"HasWeapon");
        auto* argument = CastField<FNameProperty>(property(function, L"WeaponID"));
        auto* normal = CastField<FBoolProperty>(property(function, L"bIsNormalWeapon"));
        auto* result = CastField<FBoolProperty>(property(function, L"ReturnValue"));
        if (!exact(function, 3) ||
            function->GetPathName() != L"/Script/ProjectPBZ.PBZBackPackSubsystem:HasWeapon" ||
            !input(argument) || argument->GetElementSize() != sizeof(FName) || !input(normal) ||
            !result || !result->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm)) {
            return Ownership::unknown;
        }
        FunctionParams params{function};
        argument->SetPropertyValueInContainer(params.data(), id);
        normal->SetPropertyValueInContainer(params.data(), item.category == Category::weapon);
        ctx.backpack->ProcessEvent(function, params.data());
        return result->GetPropertyValueInContainer(params.data()) ? Ownership::owned
                                                                  : Ownership::unowned;
    }
    return Ownership::unknown;
}

auto current_ownership(const Context& ctx, const CatalogItem& item) -> Ownership {
    if (item.category == Category::weapon || item.category == Category::shaqi) {
        return catalog_ownership(ctx, item);
    }
    const auto ids = item.category == Category::accessory
                         ? owned_accessories(ctx)
                         : owned_book_or_medicine_ids(ctx, item.category);
    if (!ids) {
        return Ownership::unknown;
    }
    return std::ranges::find(*ids, item.id) == ids->end() ? Ownership::unowned : Ownership::owned;
}

// The three native pickups only differ in ID and flags. Reflection still matches each full
// signature.
auto acquire_equipment(UObject* backpack, Category category, FName id, bool& issued) -> void {
    const bool weapon = category == Category::weapon || category == Category::shaqi;
    const bool medicine = category == Category::medicine;
    const wchar_t* name = weapon     ? L"AddWeaponItem"
                          : medicine ? L"PushMedicineToInventory"
                                     : L"PushAccessoryToInventory";
    auto* fn = backpack->GetFunctionByNameInChain(name);
    auto* argument = CastField<FNameProperty>(property(fn, weapon     ? L"WeaponID"
                                                           : medicine ? L"MedicineID"
                                                                      : L"AccessoryID"));
    if ((!weapon && !medicine && category != Category::accessory) ||
        !exact(fn, weapon     ? 5
                   : medicine ? 3
                              : 2) ||
        fn->GetPathName() != std::wstring{L"/Script/ProjectPBZ.PBZBackPackSubsystem:"} + name ||
        fn->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate) || !input(argument) ||
        argument->GetElementSize() != sizeof(FName)) {
        signature_error(fn, "装备或药品的获得接口不兼容。");
    }
    FunctionParams args{fn};
    argument->SetPropertyValueInContainer(args.data(), id);
    const auto flag = [&](const wchar_t* field_name, bool value) {
        auto* field = CastField<FBoolProperty>(property(fn, field_name));
        if (!input(field) || field->GetElementSize() != 1) {
            signature_error(fn, "装备或药品的获得参数不兼容。");
        }
        field->SetPropertyValueInContainer(args.data(), value);
    };
    if (weapon) {
        flag(L"bIsNormalWeapon", category == Category::weapon);
        flag(L"bPushWidget", false);
        flag(L"bIsAddByLoadData", false);
        flag(L"bAutoEquipment", false);
    } else {
        flag(L"bAutoEquip", false);
        if (medicine) {
            flag(L"bShowPickupUI", false);
        }
    }
    issued = true;
    backpack->ProcessEvent(fn, args.data());
}

[[nodiscard]] bool is_registry_receiver(UObject* receiver) {
    return receiver &&
           !receiver->HasAnyFlags(
               static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)) &&
           receiver->GetClassPrivate() &&
           receiver->GetClassPrivate()->GetPathName() == L"/Script/AssetRegistry.AssetRegistryImpl";
}

std::wstring registry_receiver() {
    auto* cdo = static_receiver(L"/Script/AssetRegistry.AssetRegistryHelpers");
    auto* fn = cdo->GetFunctionByNameInChain(L"GetAssetRegistry");
    auto* result = CastField<FInterfaceProperty>(property(fn, L"ReturnValue"));
    if (!exact(fn, 1) || !fn->HasAnyFunctionFlags(FUNC_Static) || !result ||
        !result->HasAnyPropertyFlags(CPF_ReturnParm) ||
        result->GetElementSize() != sizeof(FScriptInterface) ||
        !result->GetInterfaceClass().Get() ||
        result->GetInterfaceClass().Get()->GetPathName() !=
            L"/Script/AssetRegistry.AssetRegistry") {
        throw std::runtime_error("GetAssetRegistry unsupported return");
    }
    FunctionParams params{fn};
    cdo->ProcessEvent(fn, params.data());
    const auto value = result->GetPropertyValueInContainer(params.data());
    auto* receiver = value.GetObject();
    // The native helper selects the singleton. Engine registries can be class default objects.
    if (!value.GetInterface() || !is_registry_receiver(receiver) ||
        (receiver->HasAnyFlags(RF_ArchetypeObject) &&
         !receiver->HasAnyFlags(RF_ClassDefaultObject))) {
        throw std::runtime_error("GetAssetRegistry returned unsupported receiver");
    }
    return receiver->GetPathName();
}

UFunction* registry_function(UObject* receiver, const wchar_t* name) {
    if (!is_registry_receiver(receiver)) {
        throw std::runtime_error("Registry receiver no longer available");
    }
    auto* fn = receiver->GetFunctionByNameInChain(name);
    if (!fn || fn->GetPathName() != std::wstring(L"/Script/AssetRegistry.AssetRegistry:") + name) {
        throw std::runtime_error("Registry function owner mismatch");
    }
    return fn;
}

bool registry_loading(UObject* receiver) {
    auto* fn = registry_function(receiver, L"IsLoadingAssets");
    auto* result = CastField<FBoolProperty>(property(fn, L"ReturnValue"));
    if (!exact(fn, 1) || !result || !result->HasAnyPropertyFlags(CPF_ReturnParm)) {
        throw std::runtime_error("IsLoadingAssets unsupported signature");
    }
    FunctionParams params{fn};
    receiver->ProcessEvent(fn, params.data());
    return result->GetPropertyValueInContainer(params.data());
}

std::size_t registry_table_count(UObject* receiver) {
    auto* fn = registry_function(receiver, L"GetAssetsByClass");
    auto* list = CastField<FArrayProperty>(property(fn, L"OutAssetData"));
    if (!output(list) || list->GetArrayFlags() != EArrayPropertyFlags::None || !list->GetInner() ||
        list->GetInner()->GetArrayDim() != 1 || list->GetElementSize() != sizeof(FScriptArray)) {
        throw std::runtime_error("Registry output array unsupported");
    }
    auto* element = CastField<FStructProperty>(list->GetInner());
    auto* name = element && element->GetStruct().Get()
                     ? CastField<FNameProperty>(property(element->GetStruct().Get(), L"AssetName"))
                     : nullptr;
    auto* package =
        element && element->GetStruct().Get()
            ? CastField<FNameProperty>(property(element->GetStruct().Get(), L"PackageName"))
            : nullptr;
    auto* input_class = CastField<FStructProperty>(property(fn, L"ClassPathName"));
    auto* recursive = CastField<FBoolProperty>(property(fn, L"bSearchSubClasses"));
    auto* result = CastField<FBoolProperty>(property(fn, L"ReturnValue"));
    if (!exact(fn, 4) || !input(input_class) || !input(recursive) ||
        (!result || !result->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm)) ||
        !input_class->GetStruct().Get() ||
        input_class->GetStruct().Get()->GetPathName() != L"/Script/CoreUObject.TopLevelAssetPath" ||
        input_class->GetElementSize() != input_class->GetStruct().Get()->GetPropertiesSize() ||
        !name || !package || !element ||
        element->GetStruct().Get()->GetPathName() != L"/Script/CoreUObject.AssetData" ||
        element->GetElementSize() != element->GetStruct().Get()->GetPropertiesSize()) {
        throw std::runtime_error("GetAssetsByClass unsupported signature");
    }
    FunctionParams params{fn};
    {
        auto* path_type = input_class->GetStruct().Get();
        auto* package_field = CastField<FNameProperty>(property(path_type, L"PackageName"));
        auto* asset_field = CastField<FNameProperty>(property(path_type, L"AssetName"));
        if (!package_field || !asset_field) {
            throw std::runtime_error("Class path fields unsupported");
        }
        auto* target = input_class->ContainerPtrToValuePtr<void>(params.data());
        package_field->SetPropertyValueInContainer(target, FName{L"/Script/Engine", FNAME_Find});
        asset_field->SetPropertyValueInContainer(target, FName{L"DataTable", FNAME_Find});
        recursive->SetPropertyValueInContainer(params.data(), true);
    }
    receiver->ProcessEvent(fn, params.data());
    if (!result->GetPropertyValueInContainer(params.data())) {
        throw std::runtime_error("Registry class query returned false");
    }
    FScriptArrayHelper_InContainer entries{list, params.data()};
    const auto count = entries.Num();
    if (count < 0 || count > 4096) {
        throw std::runtime_error("Registry table count outside scan bounds");
    }
    return static_cast<std::size_t>(count);
}

}  // namespace

auto ItemRuntime::start_refresh(UEngine* engine, const Request& request) -> void {
    const auto ctx = context(engine);
    Refresh job;
    job.generation = request.generation;
    job.world = ctx.world->GetPathName();
    const auto backpack_path = ctx.backpack->GetPathName();
    job.objects = all_items(ctx);
    const auto after = context(engine);
    if (after.world->GetPathName() != job.world || after.backpack->GetPathName() != backpack_path) {
        throw std::runtime_error("读取列表期间背包所属世界已改变。");
    }
    std::ranges::sort(job.objects);
    job.objects.erase(std::unique(job.objects.begin(), job.objects.end()), job.objects.end());
    job.rows.reserve(job.objects.size());
    refresh_ = std::move(job);
}

auto ItemRuntime::continue_refresh(UEngine* engine) -> void {
    constexpr auto kFrameBudget = std::chrono::milliseconds{2};
    constexpr std::size_t kItemsPerFrame = 16;
    const auto start = std::chrono::steady_clock::now();
    std::size_t processed{};
    auto job = std::move(*refresh_);
    refresh_.reset();  // A nested LoadMap callback must not invalidate an active job reference.
    while (job.next < job.objects.size() && processed < kItemsPerFrame) {
        job.rows.push_back(read_item(engine, job.objects[job.next], job.world));
        if (state_->closed() || state_->snapshot()->generation != job.generation) {
            return;
        }
        ++job.next;
        ++processed;
        if (std::chrono::steady_clock::now() - start >= kFrameBudget) {
            break;
        }
    }
    if (job.next == job.objects.size()) {
        std::ranges::sort(job.rows, {}, &Item::id);
        if (std::adjacent_find(job.rows.begin(), job.rows.end(),
                               [](const auto& left, const auto& right) {
                                   return left.id == right.id;
                               }) != job.rows.end()) {
            throw std::runtime_error("背包中物品 ID 不唯一，读取已拒绝。");
        }
        auto result = *state_->snapshot();
        result.generation = job.generation;
        result.world = std::move(job.world);
        result.items = std::move(job.rows);
        result.phase = Phase::ready;
        result.message = "背包读取完成。选择普通道具后输入目标数量。";
        state_->publish(std::move(result));
    } else {
        refresh_ = std::move(job);
    }
}

auto ItemRuntime::apply_quantity(UEngine* engine, const Request& request) -> void {
    auto result = *state_->snapshot();
    result.phase = Phase::ready;
    const auto ctx = context(engine);
    if (ctx.world->GetPathName() != request.world) {
        throw std::runtime_error("所选背包的世界已改变，请重新读取。");
    }
    const auto members = all_items(ctx);
    if (std::ranges::find(members, request.original.object_path) == members.end()) {
        throw std::runtime_error("所选物品已不在当前背包中。");
    }
    const auto original = read_item(engine, request.original.object_path, request.world);
    const auto decision = decide(original, request.original.count, request.target);
    if (original.id != request.original.id || decision == Decision::reject) {
        throw std::runtime_error("道具或数量已变化，请刷新后重新提交。");
    }
    if (decision == Decision::unchanged) {
        result.message = "数量未变化，没有执行写入。";
        state_->publish(std::move(result));
        return;
    }
    const auto preflight = context(engine);
    if (preflight.world->GetPathName() != request.world) {
        throw std::runtime_error("预检期间世界已改变。");
    }
    validate_delta(preflight.backpack, true);
    validate_delta(preflight.backpack, false);
    auto* object = inventory_object(original.object_path);
    auto* original_max = field<FIntProperty>(object, STR("HasMaxAmount"));
    auto* id_property = field<FNameProperty>(object, STR("ItemId"));
    if (original_max == nullptr || id_property == nullptr) {
        throw std::runtime_error("无法预检数量恢复所需的属性。");
    }
    const auto id = id_property->GetPropertyValueInContainer(object);
    bool write_started{};
    try {
        const auto current = context(engine);
        if (state_->closed() || current.world->GetPathName() != request.world) {
            throw std::runtime_error("写入前世界已改变。");
        }
        write_started = true;
        native_delta(current.backpack, id, request.target - original.count);
        const auto after = read_item(engine, original.object_path, request.world);
        if (after.id != original.id || after.count != request.target || !editable(after)) {
            throw std::runtime_error("原生增减后数量未达到目标。");
        }
        const auto row = std::ranges::find(result.items, original.id, &Item::id);
        if (row != result.items.end()) {
            *row = after;
        }
        result.catalog.reset();
        result.catalog_message = "背包数量已变化，请刷新目录。";
        result.message = "修改成功，已重新读取并确认数量。";
        RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor quantity verified: {} {} -> {}\n"),
                                                id.ToString(), original.count, after.count);
    } catch (...) {
        if (!write_started) {
            throw;
        }
        bool restored{};
        try {
            const auto after = read_item(engine, original.object_path, request.world);
            if (!rollback_owned(original, after, request.target)) {
                throw std::runtime_error("当前值超出本事务影响范围，不能用旧快照覆盖。");
            }
            const auto current = context(engine);
            if (after.count != original.count) {
                native_delta(current.backpack, id, original.count - after.count);
            }
            const auto before_peak = read_item(engine, original.object_path, request.world);
            if (before_peak.count != original.count ||
                !rollback_owned(original, before_peak, request.target)) {
                throw std::runtime_error("恢复期间物品值已发生其他变化。");
            }
            object = inventory_object(original.object_path);
            auto* maximum = field<FIntProperty>(object, STR("HasMaxAmount"));
            if (maximum == nullptr) {
                throw std::runtime_error("恢复属性已失效。");
            }
            maximum->SetPropertyValueInContainer(object, original.historical_max);
            const auto verified = read_item(engine, original.object_path, request.world);
            restored = verified.count == original.count &&
                       verified.historical_max == original.historical_max;
        } catch (...) {
            restored = false;
        }
        result.message = restored ? "写入未通过验证，原数量已恢复并确认。"
                                  : "恢复无法确认；保留原值记录并停用后续物品修改。";
        if (!restored) {
            result.recovery = Recovery{request.world, original, request.target};
        }
        RC::Output::send<RC::LogLevel::Error>(
            STR("PBZEditor quantity rollback: {} original={} max={} verified={}\n"), id.ToString(),
            original.count, original.historical_max, restored);
    }
    state_->publish(std::move(result));
}

auto ItemRuntime::grant_item(UEngine* engine, const Request& request) -> void {
    auto result = *state_->snapshot();
    result.phase = result.items.empty() ? Phase::unavailable : Phase::ready;
    if (!result.catalog || request.generation != result.generation ||
        request.row_index >= result.catalog->rows.size() ||
        request.world != result.catalog->world || request.backpack != result.catalog->backpack) {
        throw std::runtime_error("目录已变化，请重新读取。");
    }
    const auto& selected = result.catalog->rows[request.row_index];
    const auto id = grant_identity(selected);
    const auto active = [&] {
        return !state_->closed() && state_->snapshot()->generation == request.generation;
    };
    const bool save = request.grant_mode == GrantMode::save_confirmed_slot;
    std::wstring slot;
    if (save) {
        slot = confirmed_slot(engine, request);
        if (slot.empty() || slot != result.catalog->slot) {
            throw std::runtime_error("当前载入槽与目录记录的继续槽位无法匹配，请重新读取。");
        }
        static_cast<void>(prepare_or_request_save(grant_context(engine, request), slot, false));
    }

    const bool counted =
        selected.category == Category::items || selected.category == Category::book;
    if (!counted &&
        current_ownership(grant_context(engine, request), selected) != Ownership::unowned) {
        throw std::runtime_error("该物品已持有或暂时无法核对，请刷新目录。");
    }
    if (counted) {
        const auto before = grant_inventory(engine, request, id);
        if (before.count != 0 || before.historical_max) {
            throw std::runtime_error("已持有该道具或其历史记录，请刷新目录。");
        }
    }
    auto catalog = std::make_shared<Catalog>(*result.catalog);
    if (!active()) {
        throw std::runtime_error("目录已变化，请重新读取。");
    }
    Item original;
    original.id = selected.id;
    const auto current = grant_context(engine, request);
    bool issued{};
    try {
        if (counted) {
            validate_delta(current.backpack, true);
            issued = true;
            native_delta(current.backpack, id, 1);
        } else {
            acquire_equipment(current.backpack, selected.category, id, issued);
        }
        if (counted) {
            const auto after = grant_inventory(engine, request, id);
            if (after.count == 0 && !after.historical_max) {
                result.catalog_message = "游戏未发放该道具。";
                state_->publish(std::move(result));
                return;
            }
            if (after.count != 1 || after.historical_max != 1) {
                throw std::runtime_error("数量无法验证。");
            }
        } else if (current_ownership(grant_context(engine, request), selected) !=
                   Ownership::owned) {
            throw std::runtime_error("未能在对应的持有列表中确认新条目。");
        }
    } catch (...) {
        if (!issued) {
            throw;
        }
        result.recovery = Recovery{request.world, std::move(original), 1};
        result.catalog_message = "获得后的背包状态无法确认，已停用物品写入。";
        result.message = result.catalog_message;
        state_->publish(std::move(result));
        RC::Output::send<RC::LogLevel::Error>(
            STR("PBZEditor grant: inventory verification failed.\n"));
        return;
    }

    catalog->rows[request.row_index].ownership = Ownership::owned;
    result.catalog = std::move(catalog);
    if (!result.items.empty()) {
        result.items.clear();
        result.phase = Phase::unavailable;
        result.message = "背包已变化，请重新读取背包。";
    }
    RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor grant verified: {} 0 -> 1\n"),
                                            id.ToString());
    if (!save) {
        result.catalog_message = "已获得 " + original.id + "，持有状态已确认。请使用游戏内存档。";
        state_->publish(std::move(result));
        return;
    }
    result.catalog_message = "已获得 1 个。游戏未接受保存请求。";
    try {
        if (active() && confirmed_slot(engine, request) == slot && active()) {
            switch (prepare_or_request_save(grant_context(engine, request), slot, true)) {
                case SaveDisposition::requested:
                    result.catalog_message = "已获得 1 个，游戏已受理保存请求。";
                    break;
                case SaveDisposition::suspended:
                    result.catalog_message = "已获得 1 个，游戏暂缓保存。";
                    break;
                default:
                    break;
            }
        }
    } catch (...) {
        result.catalog_message = "已获得 1 个，游戏保存状态无法确认。";
    }
    state_->publish(std::move(result));
}

auto ItemRuntime::start_catalog(UEngine* engine, const Request& request) -> void {
    const auto ctx = context(engine);
    catalog_ = CatalogScan{.generation = request.generation,
                           .world = ctx.world->GetPathName(),
                           .started = std::chrono::steady_clock::now(),
                           .backpack = ctx.backpack->GetPathName()};
}

auto ItemRuntime::continue_catalog(UEngine* engine) -> void {
    auto job = std::move(*catalog_);
    catalog_.reset();
    const auto start = std::chrono::steady_clock::now();
    const auto active = [&] {
        return !state_->closed() && state_->snapshot()->generation == job.generation;
    };
    if (!active()) {
        return;
    }
    const auto ctx = context(engine);
    if (ctx.world->GetPathName() != job.world || ctx.backpack->GetPathName() != job.backpack) {
        throw std::runtime_error("世界或背包已改变，请刷新目录。");
    }
    if (start - job.started > std::chrono::seconds{30}) {
        job.result.failed_rows += job.keys.size() - job.next;
        job.result.failed_sources += kCatalogSources.size() - job.source;
        job.source = kCatalogSources.size();
    }
    if (!job.registry_checked) {
        // Catalog coverage remains partial until the enumerated candidates have been classified.
        job.registry_checked = true;
        try {
            const auto path = registry_receiver();
            if (active() && !registry_loading(find_object(path)) && active()) {
                job.result.registry_tables = registry_table_count(find_object(path));
            }
        } catch (const std::exception&) {
            RC::Output::send<RC::LogLevel::Warning>(
                STR("PBZEditor catalog registry unavailable\n"));
        }
        if (active()) {
            catalog_ = std::move(job);
        }
        return;
    }
    if (job.source < kCatalogSources.size()) {
        const auto& source = kCatalogSources[job.source];
        if (!job.source_ready) {
            try {
                if (!job.schema_checked) {
                    static_cast<void>(validate_table(source, false));
                    job.schema_checked = true;
                    if (active()) {
                        catalog_ = std::move(job);
                    }
                    return;
                }
                job.keys = validate_table(source, true);
                job.result.raw += job.keys.size();
                job.source_ready = true;
            } catch (const std::exception& error) {
                ++job.result.failed_sources;
                RC::Output::send<RC::LogLevel::Warning>(
                    STR("PBZEditor catalog source {}: {}\n"), source.path,
                    std::wstring(error.what(),
                                 error.what() + std::char_traits<char>::length(error.what())));
            }
        } else if ((source.category == Category::medicine ||
                    source.category == Category::accessory || source.category == Category::book) &&
                   !job.ownership_checked) {
            job.ownership_checked = true;
            try {
                job.owned_ids = source.category == Category::accessory
                                    ? owned_accessories(ctx)
                                    : owned_book_or_medicine_ids(ctx, source.category);
            } catch (const std::exception&) {
                RC::Output::send<RC::LogLevel::Warning>(
                    STR("PBZEditor catalog ownership unavailable {}\n"), source.path);
            }
        } else {
            for (std::size_t processed = 0; job.next < job.keys.size() && processed < 16;
                 ++processed) {
                if (!active()) {
                    return;
                }
                if (processed != 0 &&
                    std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{2}) {
                    break;
                }
                const auto& key = job.keys[job.next];
                try {
                    if (!job.pending_row) {
                        job.pending_row = catalog_row(source, key, context(engine));
                        if (job.pending_row) {
                            continue;
                        }
                        ++job.result.excluded;
                        RC::Output::send<RC::LogLevel::Verbose>(
                            STR("PBZEditor catalog excluded {} {}\n"), source.path, key);
                    } else if (!job.description_checked) {
                        job.description_checked = true;
                        if (source.description) {
                            job.pending_row->description =
                                catalog_text(source, key, context(engine), source.description,
                                             job.pending_row->id);
                        }
                        continue;
                    } else {
                        auto& item = *job.pending_row;
                        if (job.owned_ids) {
                            item.ownership =
                                std::ranges::find(*job.owned_ids, item.id) == job.owned_ids->end()
                                    ? Ownership::unowned
                                    : Ownership::owned;
                        } else {
                            item.ownership = catalog_ownership(context(engine), item);
                        }
                        job.result.rows.push_back(std::move(item));
                    }
                } catch (const std::exception&) {
                    ++job.result.failed_rows;
                    RC::Output::send<RC::LogLevel::Warning>(
                        STR("PBZEditor catalog row failed {} {}\n"), source.path, key);
                }
                job.pending_row.reset();
                job.description_checked = false;
                ++job.next;
                if (!active()) {
                    return;
                }
                if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{2}) {
                    break;
                }
            }
        }
        if (!active()) {
            return;
        }
        if (job.next == job.keys.size()) {
            RC::Output::send<RC::LogLevel::Verbose>(STR("PBZEditor catalog checked {} keys={}\n"),
                                                    source.path, job.keys.size());
            job.keys.clear();
            job.next = 0;
            ++job.source;
            job.source_ready = false;
            job.schema_checked = false;
            job.ownership_checked = false;
            job.owned_ids.reset();
        }
        catalog_ = std::move(job);
        return;
    }
    // Keep conflicting rows separately. Merge only identical display records from one source.
    auto& rows = job.result.rows;
    std::ranges::sort(rows, {}, [](const CatalogItem& item) {
        return std::tie(item.category, item.id, item.name, item.description, item.source,
                        item.ownership, item.row_key);
    });
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].category == rows[i - 1].category && rows[i].id == rows[i - 1].id) {
            rows[i - 1].grantable_definition = false;
            rows[i].grantable_definition = false;
        }
    }
    const auto aliases = std::ranges::unique(rows, [](const auto& left, const auto& right) {
        const bool same =
            std::tie(left.category, left.id, left.name, left.description, left.source,
                     left.ownership) == std::tie(right.category, right.id, right.name,
                                                 right.description, right.source, right.ownership);
        if (same) {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("PBZEditor catalog alias {} {} -> {}\n"), RC::ensure_str(right.source),
                RC::ensure_str(right.row_key), RC::ensure_str(left.row_key));
        }
        return same;
    });
    job.result.aliases = static_cast<std::size_t>(std::ranges::distance(aliases));
    rows.erase(aliases.begin(), aliases.end());
    auto result = *state_->snapshot();
    result.phase = result.items.empty() ? Phase::unavailable : Phase::ready;
    result.catalog_message = "部分目录 · 展示 " + std::to_string(job.result.rows.size()) + " / " +
                             std::to_string(job.result.raw) + " 行 · 排除 " +
                             std::to_string(job.result.excluded) + " · 失败 " +
                             std::to_string(job.result.failed_rows) + " 行 / " +
                             std::to_string(job.result.failed_sources) + " 来源";
    if (job.result.registry_tables != 0) {
        result.catalog_message +=
            " · 注册表候选 " + std::to_string(job.result.registry_tables) + " 张表";
    }
    RC::Output::send<RC::LogLevel::Verbose>(
        STR("PBZEditor catalog finished raw={} shown={} aliases={} excluded={} failed={} "
            "sources_failed={} "
            "registry={}\n"),
        job.result.raw, job.result.rows.size(), job.result.aliases, job.result.excluded,
        job.result.failed_rows, job.result.failed_sources, job.result.registry_tables);
    try {
        job.result.slot = confirmed_slot(
            engine,
            Request{.generation = job.generation, .world = job.world, .backpack = job.backpack});
    } catch (const std::exception&) {
        job.result.slot.clear();
    }
    if (!active()) {
        return;
    }
    job.result.slot_label = utf8(job.result.slot);
    job.result.world = std::move(job.world);
    job.result.backpack = std::move(job.backpack);
    result.catalog = std::make_shared<const Catalog>(std::move(job.result));
    state_->publish(std::move(result));
}

auto ItemRuntime::tick(UEngine* engine) noexcept -> void {
    if (!catalog_ && !refresh_ && !state_->has_request()) {
        return;
    }
    if (state_->closed()) {
        catalog_.reset();
        refresh_.reset();
        return;
    }
    bool was_catalog = catalog_.has_value();
    try {
        if (auto request = state_->take_request()) {
            was_catalog =
                request->kind == Request::Kind::catalog || request->kind == Request::Kind::grant;
            if (request->kind == Request::Kind::refresh) {
                start_refresh(engine, *request);
            } else if (request->kind == Request::Kind::catalog) {
                start_catalog(engine, *request);
            } else if (request->kind == Request::Kind::grant) {
                grant_item(engine, *request);
            } else {
                apply_quantity(engine, *request);
            }
        }
        if (catalog_) {
            continue_catalog(engine);
            return;
        }
        if (refresh_) {
            continue_refresh(engine);
        }
    } catch (const std::exception& error) {
        refresh_.reset();
        catalog_.reset();
        try {
            auto result = *state_->snapshot();
            result.phase = result.items.empty() ? Phase::unavailable : Phase::ready;
            if (was_catalog) {
                result.catalog_message = error.what();
            } else {
                result.message = error.what();
            }
            state_->publish(std::move(result));
            RC::Output::send<RC::LogLevel::Warning>(
                STR("PBZEditor items: request failed; inspect the GUI message.\n"));
        } catch (...) {
            state_->close();
        }
    } catch (...) {
        catalog_.reset();
        refresh_.reset();
        state_->close();
    }
}

auto ItemRuntime::begin_world_change() noexcept -> void {
    catalog_.reset();
    refresh_.reset();
    try {
        state_->begin_world_change();
    } catch (...) {
        state_->close();
    }
}
}  // namespace pbz::items
