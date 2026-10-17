/*
 * Druide — Equilibre (0), Farouche (1), Restauration (2).
 * Priorités d'après les guides Wowhead WotLK Classic.
 * Farouche : la rotation suit la forme actuelle (ours = tank, sinon félin).
 */

#include "Rotation.h"
#include "GameTime.h"
#include "Creature.h"
#include "Map.h"
#include "SpellAuras.h"

namespace ModRotation
{
namespace
{
    constexpr uint32 MOONKIN_FORM   = 24858;
    constexpr uint32 CAT_FORM       = 768;

    constexpr uint32 FAERIE_FIRE    = 770;
    constexpr uint32 FAERIE_FIRE_FERAL = 16857;
    constexpr uint32 INSECT_SWARM   = 5570;
    constexpr uint32 MOONFIRE       = 8921;
    constexpr uint32 WRATH          = 5176;
    constexpr uint32 STARFIRE       = 2912;
    constexpr uint32 STARFALL       = 48505;
    constexpr uint32 HURRICANE      = 16914;
    constexpr uint32 FORCE_OF_NATURE = 33831;

    constexpr uint32 MANGLE_CAT     = 33876;
    constexpr uint32 MANGLE_BEAR    = 33878;
    constexpr uint32 RAKE           = 1822;
    constexpr uint32 RIP            = 1079;
    constexpr uint32 SHRED          = 5221;
    constexpr uint32 CLAW           = 1082;
    constexpr uint32 SAVAGE_ROAR    = 52610;
    constexpr uint32 TIGERS_FURY    = 5217;
    constexpr uint32 LACERATE       = 33745;
    constexpr uint32 MAUL           = 6807;
    constexpr uint32 SWIPE_BEAR     = 779;

    constexpr uint32 REJUVENATION   = 774;
    constexpr uint32 REGROWTH       = 8936;
    constexpr uint32 LIFEBLOOM      = 33763;
    constexpr uint32 WILD_GROWTH    = 48438;
    constexpr uint32 NOURISH        = 50464;
    constexpr uint32 HEALING_TOUCH  = 5185;
    constexpr uint32 SWIFTMEND      = 18562;

    constexpr uint32 AURA_ECLIPSE_LUNAR = 48518; // Feu stellaire +40 % critique
    constexpr uint32 AURA_ECLIPSE_SOLAR = 48517; // Colère +40 % dégâts
    constexpr uint32 AURA_BLOODLUST     = 2825;
    constexpr uint32 AURA_HEROISM       = 32182;

    constexpr uint32 ECLIPSE_TALENT       = 48516;
    constexpr uint32 IMPROVED_FAERIE_FIRE = 33600;
    constexpr uint32 ECLIPSE_ICD_MS       = 30000;
    constexpr float  STARFALL_RADIUS      = 30.0f;
    constexpr size_t DANGER_ATTACKERS     = 3;     // en solo : tréants de secours
    constexpr float  DANGER_HEALTH_PCT    = 40.0f;

    // Le talent Eclipse a un temps de recharge interne de 30 s par type
    // d'Eclipse, gardé privé par le script du cœur (spell_dru_eclipse) : on le
    // reconstitue à partir des auras observées à chaque appui.
    struct EclipseState : public DataMap::Base
    {
        uint32 lunarProc = 0; // GameTime en ms du dernier déclenchement
        uint32 solarProc = 0;
        bool   lunarSeen = false;
        bool   solarSeen = false;
    };

    void RecordEclipse(Player* me, uint32 auraId, uint32 now, uint32& procTime, bool& seen)
    {
        if (Aura* aura = me->GetAura(auraId))
        {
            procTime = now - uint32(aura->GetMaxDuration() - aura->GetDuration());
            seen = true;
        }
    }

    uint32 EclipseCooldownLeft(uint32 procTime, bool seen, uint32 now)
    {
        if (!seen)
            return 0;

        uint32 elapsed = now - procTime;
        return elapsed < ECLIPSE_ICD_MS ? ECLIPSE_ICD_MS - elapsed : 0;
    }

    // Choix du sort principal selon le cycle d'Eclipse :
    //  - Eclipse lunaire  → Feu stellaire (+40 % de critique)
    //  - Eclipse solaire  → Colère (+40 % de dégâts)
    //  - hors Eclipse : on « pêche » celle dont le temps de recharge interne
    //    finit le plus tôt — Colère déclenche la lunaire, Feu stellaire la
    //    solaire. Après une lunaire on reste donc sur Feu stellaire jusqu'à
    //    la solaire, et inversement.
    // Talent passif (aura permanente) appris, quel que soit son rang.
    bool HasTalent(Player* me, uint32 firstRankId)
    {
        return me->GetAuraOfRankedSpell(firstRankId) != nullptr;
    }

    uint32 PickNuke(Player* me)
    {
        if (!HasTalent(me, ECLIPSE_TALENT) || !MaxRank(me, STARFIRE))
            return WRATH;

        if (me->HasAura(AURA_ECLIPSE_LUNAR))
            return STARFIRE;
        if (me->HasAura(AURA_ECLIPSE_SOLAR))
            return WRATH;

        EclipseState* state = me->CustomData.GetDefault<EclipseState>("mod_rotation_eclipse");
        uint32 now = GameTime::GetGameTimeMS().count();

        uint32 lunarIn = EclipseCooldownLeft(state->lunarProc, state->lunarSeen, now);
        uint32 solarIn = EclipseCooldownLeft(state->solarProc, state->solarSeen, now);

        return solarIn < lunarIn ? STARFIRE : WRATH;
    }

    // --- Situations -------------------------------------------------------

    // Jeu en groupe : en groupe, ou dans un donjon/raid (IsDungeon couvre
    // les deux). Hors de ce cadre, la rotation est celle du jeu en solo.
    bool InGroupContent(Player* me)
    {
        return me->GetGroup() || me->GetMap()->IsDungeon();
    }

    // Cible qui vit assez longtemps pour rentabiliser un GCD de préparation :
    // élite, boss ou joueur.
    bool IsToughTarget(Unit* target)
    {
        if (Creature* creature = target->ToCreature())
            return creature->isElite() || creature->isWorldBoss() || creature->IsDungeonBoss();

        return target->IsPlayer();
    }

    // Boss de donjon (flag du cœur) ou de rang « boss » (raids, boss du monde).
    bool IsBoss(Unit* target)
    {
        Creature* creature = target->ToCreature();
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }

    // Plusieurs ennemis sur le druide, ou il fond.
    bool InDanger(Player* me)
    {
        return me->getAttackers().size() >= DANGER_ATTACKERS
            || me->GetHealthPct() < DANGER_HEALTH_PCT;
    }

    bool HasBloodlust(Player* me)
    {
        return me->HasAura(AURA_BLOODLUST) || me->HasAura(AURA_HEROISM);
    }

    // --- Étapes -----------------------------------------------------------
    // Chacune renvoie true si elle a lancé un sort : l'appui est consommé.

    bool EnterMoonkinForm(Player* me)
    {
        return me->HasSpell(MOONKIN_FORM) && me->GetShapeshiftForm() != FORM_MOONKIN
            && TryCast(me, me, MOONKIN_FORM);
    }

    // Mémorise les Eclipses en cours pour reconstituer leur recharge interne.
    void TrackEclipses(Player* me)
    {
        if (!HasTalent(me, ECLIPSE_TALENT))
            return;

        EclipseState* state = me->CustomData.GetDefault<EclipseState>("mod_rotation_eclipse");
        uint32 now = GameTime::GetGameTimeMS().count();
        RecordEclipse(me, AURA_ECLIPSE_LUNAR, now, state->lunarProc, state->lunarSeen);
        RecordEclipse(me, AURA_ECLIPSE_SOLAR, now, state->solarProc, state->solarSeen);
    }

    // Force de la nature : 3 tréants pendant 30 s, 3 min de recharge.
    bool SummonTreants(Ctx& c)
    {
        return TryCastAt(c.me, c.target, FORCE_OF_NATURE);
    }

    // N'importe quelle Lucioles déjà posée (farouche, autre druide) suffit.
    bool ApplyFaerieFire(Ctx& c)
    {
        return !c.target->GetAuraOfRankedSpell(FAERIE_FIRE)
            && !c.target->GetAuraOfRankedSpell(FAERIE_FIRE_FERAL)
            && TryCastRank(c.me, c.target, FAERIE_FIRE);
    }

    // Météores à chaque recharge, si la cible est à portée des étoiles et
    // qu'aucun ennemi paisible alentour ne risque d'être pullé.
    bool CastStarfall(Ctx& c)
    {
        return c.me->IsWithinDistInMap(c.target, STARFALL_RADIUS)
            && !AnyIdleEnemyNear(c.me, STARFALL_RADIUS, c.target)
            && TryCast(c.me, c.me, MaxRank(c.me, STARFALL));
    }

    // Ouragan à partir de 3 cibles.
    bool CastHurricane(Ctx& c)
    {
        return c.enemies >= std::max<uint32>(config.AoeThreshold, 3)
            && TryCastAt(c.me, c.target, HURRICANE);
    }

    // Essaim d'insectes entretenu, sauf pendant l'Eclipse lunaire : chaque
    // GCD y va au Feu stellaire renforcé (pendant la solaire, on le garde).
    bool KeepInsectSwarm(Ctx& c)
    {
        return !c.me->HasAura(AURA_ECLIPSE_LUNAR)
            && KeepAuraOn(c.me, c.target, INSECT_SWARM);
    }

    // En mouvement, seuls les instantanés : DoTs entretenus, puis Feu lunaire
    // en remplisseur.
    void CastWhileMoving(Ctx& c)
    {
        if (KeepAuraOn(c.me, c.target, INSECT_SWARM)) return;
        if (KeepAuraOn(c.me, c.target, MOONFIRE)) return;
        TryCastRank(c.me, c.target, MOONFIRE);
    }

    // Colère ou Feu stellaire selon le cycle d'Eclipse, l'autre en repli
    // (sort pas encore appris, etc.).
    void CastNuke(Ctx& c)
    {
        uint32 nuke = PickNuke(c.me);
        if (TryCastRank(c.me, c.target, nuke)) return;
        TryCastRank(c.me, c.target, nuke == WRATH ? STARFIRE : WRATH);
    }

    // --- Scénarios --------------------------------------------------------

    // Groupe, donjon, raid : la rotation des guides.
    //  - Lucioles d'abord : l'armure profite à tous.
    //  - Tréants gardés pour les boss : tout le menu fretin des donjons est
    //    élite et les gaspillerait.
    //  - Feu lunaire réservé au mouvement : les GCD rapportent plus en
    //    Colère/Feu stellaire.
    void BalanceGroup(Ctx& c)
    {
        bool boss = IsBoss(c.target);

        // Sous Furie sanguinaire, l'attaque des tréants profite de la hâte.
        if (boss && HasBloodlust(c.me) && SummonTreants(c)) return;
        if (ApplyFaerieFire(c)) return;
        if (CastStarfall(c)) return;
        // Sinon dès la recharge : sur un boss, c'est le pull, là où la Furie
        // sanguinaire est le plus souvent lancée.
        if (boss && SummonTreants(c)) return;

        if (c.me->isMoving())
        {
            CastWhileMoving(c);
            return;
        }

        if (CastHurricane(c)) return;
        if (KeepInsectSwarm(c)) return;
        // Sans le talent Eclipse (montée de niveau), Feu lunaire vaut son GCD.
        if (!HasTalent(c.me, ECLIPSE_TALENT) && KeepAuraOn(c.me, c.target, MOONFIRE)) return;
        CastNuke(c);
    }

    // Solo : pas de préparation sur le menu fretin.
    //  - Lucioles et tréants seulement sur les cibles coriaces.
    //  - Tréants de secours quand le druide est en danger : ils détournent
    //    les ennemis.
    //  - Feu lunaire entretenu.
    void BalanceSolo(Ctx& c)
    {
        bool tough = IsToughTarget(c.target);

        if (InDanger(c.me) && SummonTreants(c)) return;
        if (tough && HasBloodlust(c.me) && SummonTreants(c)) return;
        // Sans groupe, l'armure ne sert à personne : seul compte le +3 % de
        // toucher/critique des sorts de Lucioles améliorées.
        if (tough && HasTalent(c.me, IMPROVED_FAERIE_FIRE) && ApplyFaerieFire(c)) return;
        if (CastStarfall(c)) return;
        if (tough && SummonTreants(c)) return;

        if (c.me->isMoving())
        {
            CastWhileMoving(c);
            return;
        }

        if (CastHurricane(c)) return;
        if (KeepInsectSwarm(c)) return;
        if (KeepAuraOn(c.me, c.target, MOONFIRE)) return;
        CastNuke(c);
    }

    void Balance(Ctx& c)
    {
        if (EnterMoonkinForm(c.me))
            return;

        // Même sans cible, pour ne manquer aucune Eclipse.
        TrackEclipses(c.me);

        if (!c.target)
            return;

        if (InGroupContent(c.me))
            BalanceGroup(c);
        else
            BalanceSolo(c);
    }

    void FeralBear(Ctx& c)
    {
        if (!c.inMelee)
            return;

        // Mutiler (ours) > Lucioles (farouche) > Lacérer x5 > Balayage (AoE) > Mutilation (dump)
        if (TryCastRank(c.me, c.target, MANGLE_BEAR)) return;
        if (TryCastRank(c.me, c.target, FAERIE_FIRE_FERAL)) return;

        Aura* lacerate = c.target->GetAuraOfRankedSpell(LACERATE, c.me->GetGUID());
        if ((!lacerate || lacerate->GetStackAmount() < 5) && TryCastRank(c.me, c.target, LACERATE))
            return;

        if (c.Aoe() && TryCastRank(c.me, c.target, SWIPE_BEAR))
            return;

        if (!c.me->GetCurrentSpell(CURRENT_MELEE_SPELL)
            && c.me->GetPower(POWER_RAGE) >= config.RageDumpThreshold * 10)
            TryCastRank(c.me, c.target, MAUL);
    }

    void FeralCat(Ctx& c)
    {
        // Forme de félin si besoin
        if (c.me->GetShapeshiftForm() != FORM_CAT)
        {
            TryCast(c.me, c.me, CAT_FORM);
            return;
        }

        if (!c.inMelee)
            return;

        uint8 cp = c.me->GetComboPoints(c.target);

        // Fureur du tigre quand l'énergie est basse
        if (c.me->GetPower(POWER_ENERGY) < 40 && TryCast(c.me, c.me, MaxRank(c.me, TIGERS_FURY)))
            return;

        // Rugissement sauvage dès 1 point de combo
        if (cp >= 1 && !c.me->HasAura(SAVAGE_ROAR) && c.me->HasSpell(SAVAGE_ROAR)
            && TryCast(c.me, c.target, SAVAGE_ROAR))
            return;

        // Déchirure à 5 points de combo
        if (cp >= 5 && !IsBleedImmune(c.target)
            && !c.target->GetAuraOfRankedSpell(RIP, c.me->GetGUID())
            && TryCastRank(c.me, c.target, RIP))
            return;

        // Griffure et Mutiler (félin) entretenus
        if (!IsBleedImmune(c.target) && KeepAuraOn(c.me, c.target, RAKE))
            return;
        if (KeepAuraOn(c.me, c.target, MANGLE_CAT))
            return;

        // Générateur : Lambeau (dans le dos), replis Mutiler puis Griffe
        if (TryCastRank(c.me, c.target, SHRED)) return;
        if (TryCastRank(c.me, c.target, MANGLE_CAT)) return;
        TryCastRank(c.me, c.target, CLAW);
    }

    void Feral(Ctx& c)
    {
        if (!c.target)
            return;

        ShapeshiftForm form = c.me->GetShapeshiftForm();
        if (form == FORM_BEAR || form == FORM_DIREBEAR)
            FeralBear(c);
        else
            FeralCat(c);
    }

    void Restoration(Ctx& c)
    {
        Unit* ally = c.friendly;
        float pct = ally->GetHealthPct();

        if (pct < config.HealUrgentPct)
        {
            // Prompte guérison consomme Récupération/Rétablissement
            if (TryCast(c.me, ally, SWIFTMEND)) return;
            if (KeepAuraOn(c.me, ally, REJUVENATION)) return;
            if (TryCastRank(c.me, ally, NOURISH)) return;
            TryCastRank(c.me, ally, HEALING_TOUCH);
            return;
        }
        if (pct < config.HealLowPct)
        {
            if (KeepAuraOn(c.me, ally, REJUVENATION)) return;
            if (KeepAuraOn(c.me, ally, REGROWTH)) return;
            if (TryCastRank(c.me, ally, WILD_GROWTH)) return;
            if (TryCastRank(c.me, ally, NOURISH)) return;
            TryCastRank(c.me, ally, HEALING_TOUCH);
            return;
        }
        if (pct < config.HealInjuredPct)
        {
            if (KeepAuraOn(c.me, ally, REJUVENATION)) return;
            if (ally != c.me && c.me->HasSpell(LIFEBLOOM) && KeepAuraOn(c.me, ally, LIFEBLOOM)) return;
            TryCastRank(c.me, ally, NOURISH);
            return;
        }

        // Personne à soigner : contribution légère en dégâts
        if (c.target)
        {
            if (KeepAuraOn(c.me, c.target, MOONFIRE)) return;
            TryCastRank(c.me, c.target, WRATH);
        }
    }
}

void Druid(Ctx& c)
{
    if (c.target && c.tree == 1)
        EngageMelee(c);

    switch (c.tree)
    {
        case 0:  Balance(c);     break;
        case 1:  Feral(c);       break;
        default: Restoration(c); break;
    }
}
} // namespace ModRotation
