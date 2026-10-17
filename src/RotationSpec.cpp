/*
 * mod-rotation — .rotation spec: choose the specialization the rotation uses
 * instead of the talent tree with the most points.
 */

#include "Rotation.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "StringConvert.h"
#include "Util.h"

#include <array>
#include <string_view>

using namespace Acore::ChatCommands;

namespace ModRotation
{
namespace
{
    // Player setting: 0 = automatic, 1-3 = talent tree + 1. It is saved with
    // the character, so the choice survives logout.
    constexpr char SETTING_SOURCE[] = "mod-rotation";
    constexpr uint32 SETTING_SPEC = 0;

    using SpecNames = std::array<std::string_view, 3>;

    // Talent tree order of each class, as dispatched by Rotation<Class>.cpp.
    SpecNames const* GetSpecNames(uint8 playerClass)
    {
        static SpecNames const warrior     = { "arms", "fury", "protection" };
        static SpecNames const paladin     = { "holy", "protection", "retribution" };
        static SpecNames const hunter      = { "beastmastery", "marksmanship", "survival" };
        static SpecNames const rogue       = { "assassination", "combat", "subtlety" };
        static SpecNames const priest      = { "discipline", "holy", "shadow" };
        static SpecNames const deathKnight = { "blood", "frost", "unholy" };
        static SpecNames const shaman      = { "elemental", "enhancement", "restoration" };
        static SpecNames const mage        = { "arcane", "fire", "frost" };
        static SpecNames const warlock     = { "affliction", "demonology", "destruction" };
        static SpecNames const druid       = { "balance", "feral", "restoration" };

        switch (playerClass)
        {
            case CLASS_WARRIOR:      return &warrior;
            case CLASS_PALADIN:      return &paladin;
            case CLASS_HUNTER:       return &hunter;
            case CLASS_ROGUE:        return &rogue;
            case CLASS_PRIEST:       return &priest;
            case CLASS_DEATH_KNIGHT: return &deathKnight;
            case CLASS_SHAMAN:       return &shaman;
            case CLASS_MAGE:         return &mage;
            case CLASS_WARLOCK:      return &warlock;
            case CLASS_DRUID:        return &druid;
            default:                 return nullptr;
        }
    }

    uint32 GetSavedSpec(Player* p)
    {
        uint32 value = p->GetPlayerSetting(SETTING_SOURCE, SETTING_SPEC).value;
        return value <= 3 ? value : 0;
    }

    // Accepts "auto", a tree number 1-3, or a spec name or its unique prefix.
    // Returns the setting value, or nothing for unknown or ambiguous input.
    Optional<uint32> ParseSpec(std::string_view arg, SpecNames const& names)
    {
        if (StringEqualI(arg, "auto"))
            return 0;

        if (Optional<uint32> number = Acore::StringTo<uint32>(arg))
        {
            if (*number >= 1 && *number <= 3)
                return number;

            return std::nullopt;
        }

        Optional<uint32> match;
        for (uint32 i = 0; i < names.size(); ++i)
        {
            if (!StringStartsWithI(names[i], arg))
                continue;

            if (match)
                return std::nullopt;

            match = i + 1;
        }

        return match;
    }
}

uint8 ChosenTree(Player* p)
{
    if (uint32 spec = GetSavedSpec(p))
        return uint8(spec - 1);

    return p->GetMostPointsTalentTree();
}

class rotation_spec_commandscript : public CommandScript
{
public:
    rotation_spec_commandscript() : CommandScript("rotation_spec_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable rotationCommandTable =
        {
            { "spec", HandleSpecCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "rotation", rotationCommandTable }
        };

        return commandTable;
    }

    static bool HandleSpecCommand(ChatHandler* handler, Optional<std::string_view> arg)
    {
        Player* player = handler->GetSession()->GetPlayer();
        SpecNames const* names = GetSpecNames(player->getClass());
        if (!names)
            return false;

        if (arg)
        {
            Optional<uint32> spec = ParseSpec(*arg, *names);
            if (!spec)
            {
                handler->PSendSysMessage("Unknown spec '{}'. Use auto, 1-3, or one of: {}, {}, {}.",
                    *arg, (*names)[0], (*names)[1], (*names)[2]);
                handler->SetSentErrorMessage(true);
                return false;
            }

            player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_SPEC, *spec);
        }

        std::string_view name = (*names)[ChosenTree(player)];
        if (GetSavedSpec(player))
            handler->PSendSysMessage("Rotation spec: {} (set by command).", name);
        else
            handler->PSendSysMessage("Rotation spec: {} (automatic, from talents).", name);

        return true;
    }
};
} // namespace ModRotation

// Registered from AddRotationScripts() in Rotation.cpp.
void AddRotationSpecScripts()
{
    new ModRotation::rotation_spec_commandscript();
}
