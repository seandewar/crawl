/*
 *  File:     spl-damage.cc
 *  Summary:  Damage-dealing spells not already handled elsewhere.
 *            Other targeted spells are covered in spl-zap.cc.
 */

#include "AppHdr.h"

#include "spl-damage.h"
#include "externs.h"

#include "areas.h"
#include "beam.h"
#include "cloud.h"
#include "coord.h"
#include "directn.h"
#include "env.h"
#include "godconduct.h"
#include "it_use2.h"
#include "itemprop.h"
#include "items.h"
#include "libutil.h"
#include "los.h"
#include "message.h"
#include "misc.h"
#include "map_knowledge.h"
#include "mon-behv.h"
#include "mon-iter.h"
#include "mon-stuff.h"
#include "mon-util.h"
#include "ouch.h"
#include "player.h"
#include "shout.h"
#include "spl-util.h"
#include "stuff.h"
#include "transform.h"
#include "traps.h"
#include "view.h"
#include "viewchar.h"


bool fireball(int pow, bolt &beam)
{
    return (zapping(ZAP_FIREBALL, pow, beam, true));
}

void setup_fire_storm(const actor *source, int pow, bolt &beam)
{
    beam.name         = "great blast of fire";
    beam.ex_size      = 2 + (random2(pow) > 75);
    beam.flavour      = BEAM_LAVA;
    beam.real_flavour = beam.flavour;
    beam.glyph        = dchar_glyph(DCHAR_FIRED_ZAP);
    beam.colour       = RED;
    beam.beam_source  = source->mindex();
    // XXX: Should this be KILL_MON_MISSILE?
    beam.thrower      =
        source->atype() == ACT_PLAYER ? KILL_YOU_MISSILE : KILL_MON;
    beam.aux_source.clear();
    beam.obvious_effect = false;
    beam.is_beam      = false;
    beam.is_tracer    = false;
    beam.is_explosion = true;
    beam.ench_power   = pow;      // used for radius
    beam.hit          = 20 + pow / 10;
    beam.damage       = calc_dice(8, 5 + pow);
}

bool cast_fire_storm(int pow, bolt &beam)
{
    if (grid_distance(beam.target, beam.source) > beam.range)
        return (false);

    setup_fire_storm(&you, pow, beam);

    mpr("A raging storm of fire appears!");

    beam.explode(false);

    viewwindow();
    return (true);
}

// No setup/cast split here as monster hellfire is completely different.
// Sad, but needed to maintain balance - monster hellfirers get asymmetric
// torment too.
bool cast_hellfire_burst(int pow, bolt &beam)
{
    beam.name              = "burst of hellfire";
    beam.aux_source        = "burst of hellfire";
    beam.ex_size           = 1;
    beam.flavour           = BEAM_HELLFIRE;
    beam.real_flavour      = beam.flavour;
    beam.glyph             = dchar_glyph(DCHAR_FIRED_BURST);
    beam.colour            = RED;
    beam.beam_source       = MHITYOU;
    beam.thrower           = KILL_YOU;
    beam.obvious_effect    = false;
    beam.is_beam           = false;
    beam.is_explosion      = true;
    beam.ench_power        = pow;      // used for radius
    beam.hit               = 20 + pow / 10;
    beam.damage            = calc_dice(6, 30 + pow);
    beam.can_see_invis     = you.can_see_invisible();
    beam.smart_monster     = true;
    beam.attitude          = ATT_FRIENDLY;
    beam.friend_info.count = 0;
    beam.is_tracer         = true;

    beam.explode(false);

    if (beam.beam_cancelled)
    {
        canned_msg(MSG_OK);
        return (false);
    }

    mpr("You call forth a pillar of hellfire!");

    beam.is_tracer = false;
    beam.in_explosion_phase = false;
    beam.explode(true);

    return (true);
}

static bool _lightning_los(const coord_def& source, const coord_def& target)
{
    // XXX: currently bounded by circular LOS radius;
    // XXX: adapt opacity -- allow passing clouds.
    return (exists_ray(source, target, opc_solid,
                       circle_def(LOS_MAX_RADIUS, C_ROUND)));
}

void cast_chain_lightning(int pow, const actor *caster)
{
    bolt beam;

    // initialise beam structure
    beam.name           = "lightning arc";
    beam.aux_source     = "chain lightning";
    beam.beam_source    = caster->mindex();
    beam.thrower        = (caster == &you) ? KILL_YOU_MISSILE : KILL_MON_MISSILE;
    beam.range          = 8;
    beam.hit            = AUTOMATIC_HIT;
    beam.glyph          = dchar_glyph(DCHAR_FIRED_ZAP);
    beam.flavour        = BEAM_ELECTRICITY;
    beam.obvious_effect = true;
    beam.is_beam        = false;       // since we want to stop at our target
    beam.is_explosion   = false;
    beam.is_tracer      = false;

    if (const monsters *monster = caster->as_monster())
        beam.source_name = monster->name(DESC_PLAIN, true);

    bool first = true;
    coord_def source, target;

    for (source = caster->pos(); pow > 0;
         pow -= 8 + random2(13), source = target)
    {
        // infinity as far as this spell is concerned
        // (Range - 1) is used because the distance is randomised and
        // may be shifted by one.
        int min_dist = MONSTER_LOS_RANGE - 1;

        int dist;
        int count = 0;

        target.x = -1;
        target.y = -1;

        for (monster_iterator mi; mi; ++mi)
        {
            if (invalid_monster(*mi))
                continue;

            dist = grid_distance(source, mi->pos());

            // check for the source of this arc
            if (!dist)
                continue;

            // randomise distance (arcs don't care about a couple of feet)
            dist += (random2(3) - 1);

            // always ignore targets further than current one
            if (dist > min_dist)
                continue;

            if (!_lightning_los(source, mi->pos()))
                continue;

            count++;

            if (dist < min_dist)
            {
                // switch to looking for closer targets (but not always)
                if (!one_chance_in(10))
                {
                    min_dist = dist;
                    target = mi->pos();
                    count = 0;
                }
            }
            else if (target.x == -1 || one_chance_in(count))
            {
                // either first target, or new selected target at min_dist
                target = mi->pos();

                // need to set min_dist for first target case
                dist = std::max(dist, min_dist);
            }
        }

        // now check if the player is a target
        dist = grid_distance(source, you.pos());

        if (dist)       // i.e., player was not the source
        {
            // distance randomised (as above)
            dist += (random2(3) - 1);

            // select player if only, closest, or randomly selected
            if ((target.x == -1
                    || dist < min_dist
                    || (dist == min_dist && one_chance_in(count + 1)))
                && _lightning_los(source, you.pos()))
            {
                target = you.pos();
            }
        }

        if (caster == &you)
        {
            monsters *monster = monster_at(target);
            if (monster)
            {
                if (stop_attack_prompt(monster, false, you.pos()))
                    return;
            }
        }

        const bool see_source = you.see_cell( source );
        const bool see_targ   = you.see_cell( target );

        if (target.x == -1)
        {
            if (see_source)
                mpr("The lightning grounds out.");

            break;
        }

        // Trying to limit message spamming here so we'll only mention
        // the thunder at the start or when it's out of LoS.
        const char* msg = "You hear a mighty clap of thunder!";
        noisy(25, source, (first || !see_source) ? msg : NULL);
        first = false;

        if (see_source && !see_targ)
            mpr("The lightning arcs out of your line of sight!");
        else if (!see_source && see_targ)
            mpr("The lightning arc suddenly appears!");

        if (!you.see_cell_no_trans( target ))
        {
            // It's no longer in the caster's LOS and influence.
            pow = pow / 2 + 1;
        }

        beam.source = source;
        beam.target = target;
        beam.colour = LIGHTBLUE;
        beam.damage = calc_dice(5, 12 + pow * 2 / 3);

        // Be kinder to the caster.
        if (target == caster->pos())
        {
            if (!(beam.damage.num /= 2))
                beam.damage.num = 1;
            if ((beam.damage.size /= 2) < 3)
                beam.damage.size = 3;
        }
        beam.fire();
    }

    more();
}

typedef std::pair<const monsters*,int> counted_monster;
typedef std::vector<counted_monster> counted_monster_list;
static void _record_monster_by_name(counted_monster_list &list,
                                    const monsters *mons)
{
    const std::string name = mons->name(DESC_PLAIN);
    for (counted_monster_list::iterator i = list.begin(); i != list.end(); ++i)
    {
        if (i->first->name(DESC_PLAIN) == name)
        {
            i->second++;
            return;
        }
    }
    list.push_back(counted_monster(mons, 1));
}

static int _monster_count(const counted_monster_list &list)
{
    int nmons = 0;
    for (counted_monster_list::const_iterator i = list.begin();
         i != list.end(); ++i)
    {
        nmons += i->second;
    }
    return (nmons);
}

static std::string _describe_monsters(const counted_monster_list &list)
{
    std::ostringstream out;

    description_level_type desc = DESC_CAP_THE;
    for (counted_monster_list::const_iterator i = list.begin();
         i != list.end(); desc = DESC_NOCAP_THE)
    {
        const counted_monster &cm(*i);
        if (i != list.begin())
        {
            ++i;
            out << (i == list.end() ? " and " : ", ");
        }
        else
            ++i;

        const std::string name =
            cm.second > 1 ? pluralise(cm.first->name(desc))
                          : cm.first->name(desc);
        out << name;
    }
    return (out.str());
}

// Poisonous light passes right through invisible players
// and monsters, and so, they are unaffected by this spell --
// assumes only you can cast this spell (or would want to).
void cast_toxic_radiance(bool non_player)
{
    if (non_player)
        mpr("The air is filled with a sickly green light!");
    else
        mpr("You radiate a sickly green light!");

    flash_view(GREEN);
    more();
    mesclr();

    // Determine whether the player is hit by the radiance. {dlb}
    if (you.duration[DUR_INVIS])
    {
        mpr("The light passes straight through your body.");
    }
    else if (!player_res_poison())
    {
        mpr("You feel rather sick.");
        poison_player(2, "", "toxic radiance");
    }

    counted_monster_list affected_monsters;
    // determine which monsters are hit by the radiance: {dlb}
    for (monster_iterator mi(you.get_los()); mi; ++mi)
    {
        if (!mi->submerged())
        {
            // Monsters affected by corona are still invisible in that
            // radiation passes through them without affecting them. Therefore,
            // this check should not be !monster->invisible().
            if (!mi->has_ench(ENCH_INVIS))
            {
                kill_category kc = KC_YOU;
                if (non_player)
                    kc = KC_OTHER;
                bool affected =
                    poison_monster(*mi, kc, 1, false, false);

                if (coinflip() && poison_monster(*mi, kc, false, false))
                    affected = true;

                if (affected)
                    _record_monster_by_name(affected_monsters, *mi);
            }
            else if (you.can_see_invisible())
            {
                // message player re:"miss" where appropriate {dlb}
                mprf("The light passes through %s.",
                     mi->name(DESC_NOCAP_THE).c_str());
            }
        }
    }

    if (!affected_monsters.empty())
    {
        const std::string message =
            make_stringf("%s %s poisoned.",
                         _describe_monsters(affected_monsters).c_str(),
                         _monster_count(affected_monsters) == 1? "is" : "are");
        if (static_cast<int>(message.length()) < get_number_of_cols() - 2)
            mpr(message.c_str());
        else
        {
            // Exclamation mark to suggest that a lot of creatures were
            // affected.
            if (non_player)
                mpr("Nearby monsters are poisoned!");
            else
                mpr("The monsters around you are poisoned!");
        }
    }
}

void cast_refrigeration(int pow, bool non_player)
{
    if (non_player)
        mpr("Something drains the heat from around you.");
    else
        mpr("The heat is drained from your surroundings.");

    flash_view(LIGHTCYAN);
    more();
    mesclr();

    // Handle the player.
    const dice_def dam_dice(3, 5 + pow / 10);
    const int hurted = check_your_resists(dam_dice.roll(), BEAM_COLD,
            "refrigeration");

    if (hurted > 0)
    {
        mpr("You feel very cold.");
        ouch(hurted, NON_MONSTER, KILLED_BY_FREEZING);

        // Note: this used to be 12!... and it was also applied even if
        // the player didn't take damage from the cold, so we're being
        // a lot nicer now.  -- bwr
        expose_player_to_element(BEAM_COLD, 5);
    }

    // Now do the monsters.

    // First build the message.
    counted_monster_list affected_monsters;

    for (monster_iterator mi(&you); mi; ++mi)
        _record_monster_by_name(affected_monsters, *mi);

    if (!affected_monsters.empty())
    {
        const std::string message =
            make_stringf("%s %s frozen.",
                         _describe_monsters(affected_monsters).c_str(),
                         _monster_count(affected_monsters) == 1? "is" : "are");
        if (static_cast<int>(message.length()) < get_number_of_cols() - 2)
            mpr(message.c_str());
        else
        {
            // Exclamation mark to suggest that a lot of creatures were
            // affected.
            mpr("The monsters around you are frozen!");
        }
    }

    // Now damage the creatures.

    // Set up the cold attack.
    bolt beam;
    beam.flavour = BEAM_COLD;
    beam.thrower = KILL_YOU;

    for (monster_iterator mi(you.get_los()); mi; ++mi)
    {
        // Note that we *do* hurt monsters which you can't see
        // (submerged, invisible) even though you get no information
        // about it.

        // Calculate damage and apply.
        int hurt = mons_adjust_flavoured(*mi, beam, dam_dice.roll());
        if (non_player)
            mi->hurt(NULL, hurt, BEAM_COLD);
        else
            mi->hurt(&you, hurt, BEAM_COLD);

        // Cold-blooded creatures can be slowed.
        if (mi->alive()
            && mons_class_flag(mi->type, M_COLD_BLOOD)
            && coinflip())
        {
            mi->add_ench(ENCH_SLOW);
        }
    }
}

bool vampiric_drain(int pow, monsters *monster)
{
    if (monster == NULL || monster->submerged())
    {
        mpr("There isn't anything there!");
        // Cost to disallow freely locating invisible monsters.
        return (true);
    }

    if (monster->observable() && monster->undead_or_demonic())
    {
        mpr("Draining that being is not a good idea.");
        return (false);
    }

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    const bool success = !stop_attack_prompt(monster, false, you.pos());

    if (success)
    {
        set_attack_conducts(conducts, monster);

        behaviour_event(monster, ME_WHACK, MHITYOU, you.pos());
    }

    enable_attack_conducts(conducts);

    if (!success)
        return (false);

    if (!monster->alive())
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return (true);
    }

    // Monster might be invisible or player misled.
    if (monster->undead_or_demonic())
    {
        mpr("Aaaarggghhhhh!");
        dec_hp(random2avg(39, 2) + 10, false, "vampiric drain backlash");
        return (true);
    }

    if (monster->holiness() != MH_NATURAL
        || monster->res_negative_energy())
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return (true);
    }

    // The practical maximum of this is about 25 (pow @ 100). - bwr
    int hp_gain = 3 + random2avg(9, 2) + random2(pow) / 7;

        hp_gain = std::min(monster->hit_points, hp_gain);
        hp_gain = std::min(you.hp_max - you.hp, hp_gain);

    if (!hp_gain)
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return (true);
    }

    const bool mons_was_summoned = monster->is_summoned();

    monster->hurt(&you, hp_gain);

    if (monster->alive())
        print_wounds(monster);

    hp_gain /= 2;

    if (hp_gain && !mons_was_summoned)
    {
        mpr("You feel life coursing into your body.");
        inc_hp(hp_gain, false);
    }

    return (true);
}

bool burn_freeze(int pow, beam_type flavour, monsters *monster)
{
    pow = std::min(25, pow);

    if (monster == NULL || monster->submerged())
    {
        mpr("There isn't anything close enough!");
        // If there's no monster there, you still pay the costs in
        // order to prevent locating invisible monsters.
        return (true);
    }

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    const bool success = !stop_attack_prompt(monster, false, you.pos());

    if (success)
    {
        set_attack_conducts(conducts, monster);

        mprf("You %s %s.",
             (flavour == BEAM_FIRE)        ? "burn" :
             (flavour == BEAM_COLD)        ? "freeze" :
             (flavour == BEAM_MISSILE)     ? "crush" :
             (flavour == BEAM_ELECTRICITY) ? "zap"
                                           : "______",
             monster->name(DESC_NOCAP_THE).c_str());

        behaviour_event(monster, ME_ANNOY, MHITYOU);
    }

    enable_attack_conducts(conducts);

    if (!success)
        return (false);

    bolt beam;
    beam.flavour = flavour;
    beam.thrower = KILL_YOU;

    const int orig_hurted = roll_dice(1, 3 + pow / 3);
    int hurted = mons_adjust_flavoured(monster, beam, orig_hurted);
    monster->hurt(&you, hurted);

    if (monster->alive())
    {
        monster->expose_to_element(flavour, orig_hurted);
        print_wounds(monster);

        if (flavour == BEAM_COLD)
        {
            const int cold_res = monster->res_cold();
            if (cold_res <= 0)
            {
                const int stun = (1 - cold_res) * random2(2 + pow/5);
                monster->speed_increment -= stun;
            }
        }
    }

    return (true);
}

int airstrike(int pow, const dist &beam)
{
    bool success = false;

    monsters *monster = monster_at(beam.target);

    if (monster == NULL)
        canned_msg(MSG_SPELL_FIZZLES);
    else
    {
        god_conduct_trigger conducts[3];
        disable_attack_conducts(conducts);

        success = !stop_attack_prompt(monster, false, you.pos());

        if (success)
        {
            set_attack_conducts(conducts, monster);

            mprf("The air twists around and strikes %s!",
                 monster->name(DESC_NOCAP_THE).c_str());

            behaviour_event(monster, ME_ANNOY, MHITYOU);
            if (mons_is_mimic(monster->type))
                mimic_alert(monster);
        }

        enable_attack_conducts(conducts);

        if (success)
        {
            int hurted = 8 + random2(random2(4) + (random2(pow) / 6)
                           + (random2(pow) / 7));

            if (mons_flies(monster))
            {
                hurted *= 3;
                hurted /= 2;
            }

            hurted -= random2(1 + monster->ac);

            hurted = std::max(0, hurted);

            monster->hurt(&you, hurted);
            if (monster->alive())
                print_wounds(monster);
        }
    }

    return (success);
}

bool cast_bone_shards(int power, bolt &beam)
{
    if (!you.weapon() || you.weapon()->base_type != OBJ_CORPSES)
    {
        canned_msg(MSG_SPELL_FIZZLES);
        return (false);
    }

    bool success = false;

    const bool was_orc = (mons_species(you.weapon()->plus) == MONS_ORC);

    if (you.weapon()->sub_type != CORPSE_SKELETON)
    {
        mpr("The corpse collapses into a pulpy mess.");

        dec_inv_item_quantity(you.equip[EQ_WEAPON], 1);

        if (was_orc)
            did_god_conduct(DID_DESECRATE_ORCISH_REMAINS, 2);
    }
    else
    {
        // Practical max of 100 * 15 + 3000 = 4500.
        // Actual max of    200 * 15 + 3000 = 6000.
        power *= 15;
        power += mons_weight(you.weapon()->plus);

        if (!player_tracer(ZAP_BONE_SHARDS, power, beam))
            return (false);

        mpr("The skeleton explodes into sharp fragments of bone!");

        dec_inv_item_quantity(you.equip[EQ_WEAPON], 1);

        if (was_orc)
            did_god_conduct(DID_DESECRATE_ORCISH_REMAINS, 2);

        zapping(ZAP_BONE_SHARDS, power, beam);

        success = true;
    }

    return (success);
}

enum DEBRIS                 // jmf: add for shatter, dig, and Giants to throw
{
    DEBRIS_METAL,           //    0
    DEBRIS_ROCK,
    DEBRIS_STONE,
    DEBRIS_WOOD,
    DEBRIS_CRYSTAL,
    NUM_DEBRIS
};          // jmf: ...and I'll actually implement the items Real Soon Now...

// Just to avoid typing this over and over.
// Returns true if monster died. -- bwr
static bool _player_hurt_monster(monsters& m, int damage,
                                 beam_type flavour = BEAM_MISSILE)
{
    if (damage > 0)
    {
        m.hurt(&you, damage, flavour, false);

        if (m.alive())
        {
            print_wounds(&m);
            behaviour_event(&m, ME_WHACK, MHITYOU);
        }
        else
        {
            monster_die(&m, KILL_YOU, NON_MONSTER);
            return (true);
        }
    }

    return (false);
}

// Here begin the actual spells:
static int _shatter_monsters(coord_def where, int pow, int, actor *)
{
    dice_def dam_dice(0, 5 + pow / 3); // number of dice set below
    monsters *mon = monster_at(where);

    if (mon == NULL)
        return (0);

    // Removed a lot of silly monsters down here... people, just because
    // it says ice, rock, or iron in the name doesn't mean it's actually
    // made out of the substance. - bwr
    switch (mon->type)
    {
    case MONS_SILVER_STATUE: // 3/2 damage
        dam_dice.num = 4;
        break;

    case MONS_CURSE_SKULL:      // double damage
    case MONS_CLAY_GOLEM:
    case MONS_STONE_GOLEM:
    case MONS_IRON_GOLEM:
    case MONS_CRYSTAL_GOLEM:
    case MONS_ORANGE_STATUE:
    case MONS_STATUE:
    case MONS_EARTH_ELEMENTAL:
    case MONS_GARGOYLE:
        dam_dice.num = 6;
        break;

    case MONS_PULSATING_LUMP:
    case MONS_JELLY:
    case MONS_SLIME_CREATURE:
    case MONS_BROWN_OOZE:
    case MONS_AZURE_JELLY:
    case MONS_DEATH_OOZE:
    case MONS_ACID_BLOB:
    case MONS_ROYAL_JELLY:
    case MONS_OOZE:
    case MONS_JELLYFISH:
    case MONS_WATER_ELEMENTAL:
        dam_dice.num = 1;
        dam_dice.size /= 2;
        break;

    case MONS_DANCING_WEAPON:     // flies, but earth based
    case MONS_MOLTEN_GARGOYLE:
    case MONS_QUICKSILVER_DRAGON:
        // Soft, earth creatures... would normally resist to 1 die, but
        // are sensitive to this spell. - bwr
        dam_dice.num = 2;
        break;

    default:
        if (mon->is_insubstantial()) // no damage
            dam_dice.num = 0;
        else if (mons_flies(mon))    // 1/3 damage
            dam_dice.num = 1;
        else if (mon->is_icy())      // 3/2 damage
            dam_dice.num = 4;
        else if (mon->is_skeletal()) // double damage
            dam_dice.num = 6;
        else
        {
            const bool petrifying = mon->petrifying();
            const bool petrified = mon->petrified() && !petrifying;

            // Petrifying or petrified monsters can be shattered.
            if (petrifying || petrified)
                dam_dice.num = petrifying ? 4 : 6; // 3/2 or double damage
            else
                dam_dice.num = 3;
        }
        break;
    }

    int damage = std::max(0, dam_dice.roll() - random2(mon->ac));

    if (damage > 0)
        _player_hurt_monster(*mon, damage);

    return (damage);
}

static int _shatter_items(coord_def where, int pow, int, actor *)
{
    UNUSED( pow );

    int broke_stuff = 0;

    for ( stack_iterator si(where); si; ++si )
    {
        if (si->base_type == OBJ_POTIONS && !one_chance_in(10))
        {
            broke_stuff++;
            destroy_item(si->index());
        }
    }

    if (broke_stuff)
    {
        if (player_can_hear(where))
            mpr("You hear glass break.", MSGCH_SOUND);

        return 1;
    }

    return 0;
}

static int _shatter_walls(coord_def where, int pow, int, actor *)
{
    int chance = 0;

    // if not in-bounds then we can't really shatter it -- bwr
    if (!in_bounds(where))
        return 0;

    if (env.markers.property_at(where, MAT_ANY, "veto_shatter") == "veto")
        return 0;

    const dungeon_feature_type grid = grd(where);

    switch (grid)
    {
    case DNGN_SECRET_DOOR:
        if (you.see_cell(where))
            mpr("A secret door shatters!");
        chance = 100;
        break;

    case DNGN_CLOSED_DOOR:
    case DNGN_DETECTED_SECRET_DOOR:
    case DNGN_OPEN_DOOR:
        if (you.see_cell(where))
            mpr("A door shatters!");
        chance = 100;
        break;

    case DNGN_METAL_WALL:
        chance = pow / 10;
        break;

    case DNGN_ORCISH_IDOL:
    case DNGN_GRANITE_STATUE:
        chance = 50;
        break;

    case DNGN_CLEAR_STONE_WALL:
    case DNGN_STONE_WALL:
        chance = pow / 6;
        break;

    case DNGN_CLEAR_ROCK_WALL:
    case DNGN_ROCK_WALL:
    case DNGN_SLIMY_WALL:
        chance = pow / 4;
        break;

    case DNGN_GREEN_CRYSTAL_WALL:
        chance = 50;
        break;

    default:
        break;
    }

    if (x_chance_in_y(chance, 100))
    {
        noisy(30, where);

        grd(where) = DNGN_FLOOR;
        set_terrain_changed(where);

        if (grid == DNGN_ORCISH_IDOL)
            did_god_conduct(DID_DESTROY_ORCISH_IDOL, 8);

        return (1);
    }

    return (0);
}

void cast_shatter(int pow)
{
    int damage = 0;
    const bool silence = silenced(you.pos());

    if (silence)
        mpr("The dungeon shakes!");
    else
    {
        noisy(30, you.pos());
        mpr("The dungeon rumbles!", MSGCH_SOUND);
    }

    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_NONE:
    case TRAN_SPIDER:
    case TRAN_LICH:
    case TRAN_DRAGON:
    case TRAN_BAT:
        break;

    case TRAN_STATUE:           // full damage
        damage = 15 + random2avg( (pow / 5), 4 );
        break;

    case TRAN_ICE_BEAST:        // 1/2 damage
        damage = 10 + random2avg( (pow / 5), 4 ) / 2;
        break;

    case TRAN_BLADE_HANDS:      // 2d3 damage
        mpr("Your scythe-like blades vibrate painfully!");
        damage = 2 + random2avg(5, 2);
        break;

    default:
        mpr("cast_shatter(): unknown transformation in spl-damage.cc");
    }

    if (damage > 0)
        ouch(damage, NON_MONSTER, KILLED_BY_TARGETING);

    int rad = 3 + (you.skills[SK_EARTH_MAGIC] / 5);

    apply_area_within_radius(_shatter_items, you.pos(), pow, rad, 0);
    apply_area_within_radius(_shatter_monsters, you.pos(), pow, rad, 0);
    int dest = apply_area_within_radius(_shatter_walls, you.pos(),
                                        pow, rad, 0);

    if (dest && !silence)
        mpr("Ka-crash!", MSGCH_SOUND);
}

static int _ignite_poison_objects(coord_def where, int pow, int, actor *)
{
    UNUSED(pow);

    int strength = 0;

    for (stack_iterator si(where); si; ++si)
    {
        if (si->base_type == OBJ_POTIONS)
        {
            switch (si->sub_type)
            {
            // intentional fall-through all the way down
            case POT_STRONG_POISON:
                strength += 20;
            case POT_DEGENERATION:
                strength += 10;
            case POT_POISON:
                strength += 10;
                destroy_item(si->index());
            default:
                break;
            }
        }
        // FIXME: implement burning poisoned ammo
    }

    if (strength > 0)
    {
        place_cloud(CLOUD_FIRE, where,
                    strength + roll_dice(3, strength / 4), KC_YOU);
    }

    return (strength);
}

static int _ignite_poison_clouds( coord_def where, int pow, int, actor *)
{
    UNUSED(pow);

    bool did_anything = false;

    const int i = env.cgrid(where);
    if (i != EMPTY_CLOUD)
    {
        cloud_struct& cloud = env.cloud[i];
        if (cloud.type == CLOUD_STINK)
        {
            did_anything = true;
            cloud.type = CLOUD_FIRE;

            cloud.decay /= 2;

            if (cloud.decay < 1)
                cloud.decay = 1;
        }
        else if (cloud.type == CLOUD_POISON)
        {
            did_anything = true;
            cloud.type = CLOUD_FIRE;
        }
    }

    return did_anything;
}

static int _ignite_poison_monsters(coord_def where, int pow, int, actor *)
{
    bolt beam;
    beam.flavour = BEAM_FIRE;   // This is dumb, only used for adjust!

    dice_def dam_dice(0, 5 + pow/7);  // Dice added below if applicable.

    monsters *mon = monster_at(where);
    if (mon == NULL)
        return (0);

    // Monsters which have poison corpses or poisonous attacks.
    if (mons_is_poisoner(mon))
        dam_dice.num = 3;

    // Monsters which are poisoned:
    int strength = 0;

    // First check for player poison.
    mon_enchant ench = mon->get_ench(ENCH_POISON);
    if (ench.ench != ENCH_NONE)
        strength += ench.degree;

    // Strength is now the sum of both poison types
    // (although only one should actually be present at a given time).
    dam_dice.num += strength;

    int damage = dam_dice.roll();
    if (damage > 0)
    {
        damage = mons_adjust_flavoured(mon, beam, damage);
        simple_monster_message(mon, " seems to burn from within!");

        dprf("Dice: %dd%d; Damage: %d", dam_dice.num, dam_dice.size, damage);

        if (!_player_hurt_monster(*mon, damage))
        {
            // Monster survived, remove any poison.
            mon->del_ench(ENCH_POISON);
            behaviour_event(mon, ME_ALERT);
        }

        return (1);
    }

    return (0);
}

void cast_ignite_poison(int pow)
{
    flash_view(RED);

    // Poison branding becomes fire branding.
    if (you.weapon()
        && you.duration[DUR_WEAPON_BRAND]
        && get_weapon_brand(*you.weapon()) == SPWPN_VENOM)
    {
        if (set_item_ego_type(*you.weapon(), OBJ_WEAPONS, SPWPN_FLAMING))
        {
            mprf("%s bursts into flame!",
                 you.weapon()->name(DESC_CAP_YOUR).c_str());

            you.wield_change = true;

            int increase = 1 + you.duration[DUR_WEAPON_BRAND]
                               /(2 * BASELINE_DELAY);

            you.increase_duration(DUR_WEAPON_BRAND, increase, 80);
        }
    }

    int totalstrength = 0;
    bool was_wielding = false;

    for (int i = 0; i < ENDOFPACK; ++i)
    {
        item_def& item = you.inv[i];
        if (!item.defined())
            continue;

        int strength = 0;

        if (item.base_type == OBJ_MISSILES && item.special == SPMSL_POISONED)
        {
            // Burn poison ammo.
            strength = item.quantity;
            mprf("Your %s burn%s!",
                 item.name(DESC_PLAIN).c_str(), item.quantity == 1 ? "s" : "");
        }
        else if (item.base_type == OBJ_POTIONS)
        {
            // Burn poisonous potions.
            switch (item.sub_type)
            {
            case POT_STRONG_POISON:
                strength = 20 * item.quantity;
                break;
            case POT_DEGENERATION:
            case POT_POISON:
                strength = 10 * item.quantity;
                break;
            default:
                break;
            }

            if (strength)
            {
                mprf("%s explode%s!",
                     item.name(DESC_PLAIN).c_str(), item.quantity == 1 ? "s" : "");
            }
        }

        if (strength)
        {
            if (i == you.equip[EQ_WEAPON])
            {
                unwield_item();
                was_wielding = true;
            }

            item_was_destroyed(item);
            destroy_item(item);
        }

        totalstrength += strength;
    }

    if (was_wielding)
        canned_msg(MSG_EMPTY_HANDED);

    if (totalstrength)
    {
        place_cloud(
            CLOUD_FIRE, you.pos(),
            random2(totalstrength / 4 + 1) + random2(totalstrength / 4 + 1) +
            random2(totalstrength / 4 + 1) + random2(totalstrength / 4 + 1) + 1,
            KC_YOU);
    }

    int damage = 0;
    // Player is poisonous.
    if (player_mutation_level(MUT_SPIT_POISON)
        || player_mutation_level(MUT_STINGER)
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_SPIDER // poison attack
        || (!player_is_shapechanged()
            && (you.species == SP_GREEN_DRACONIAN       // poison breath
                || you.species == SP_KOBOLD             // poisonous corpse
                || you.species == SP_NAGA)))            // spit poison
    {
        damage = roll_dice(3, 5 + pow / 7);
    }

    // Player is poisoned.
    damage += roll_dice(you.duration[DUR_POISONING], 6);

    if (damage)
    {
        const int resist = player_res_fire();
        if (resist > 0)
        {
            mpr("You feel like your blood is boiling!");
            damage /= 3;
        }
        else if (resist < 0)
        {
            mpr("The poison in your system burns terribly!");
            damage *= 3;
        }
        else
            mpr("The poison in your system burns!");

        ouch(damage, NON_MONSTER, KILLED_BY_TARGETING);

        if (you.duration[DUR_POISONING] > 0)
        {
            mpr("You feel that the poison has left your system.");
            you.duration[DUR_POISONING] = 0;
        }
    }

    apply_area_visible(_ignite_poison_clouds, pow);
    apply_area_visible(_ignite_poison_objects, pow);
    apply_area_visible(_ignite_poison_monsters, pow);

#ifndef USE_TILE
    delay(100); // show a brief flash
#endif
    flash_view(0);
}

static int _discharge_monsters(coord_def where, int pow, int, actor *)
{
    monsters *monster = monster_at(where);
    int damage = 0;

    bolt beam;
    beam.flavour = BEAM_ELECTRICITY; // used for mons_adjust_flavoured

    if (where == you.pos())
    {
        mpr("You are struck by lightning.");
        damage = 3 + random2(5 + pow / 10);
        damage = check_your_resists(damage, BEAM_ELECTRICITY,
                                    "static discharge");
        if (you.airborne())
            damage /= 2;
        ouch(damage, NON_MONSTER, KILLED_BY_WILD_MAGIC);
    }
    else if (monster == NULL)
        return (0);
    else if (monster->res_elec() > 0 || mons_flies(monster))
        return (0);
    else
    {
        damage = 3 + random2(5 + pow/10);
        damage = mons_adjust_flavoured(monster, beam, damage );

        if (damage)
        {
            mprf("%s is struck by lightning.",
                 monster->name(DESC_CAP_THE).c_str());
            _player_hurt_monster(*monster, damage);
        }
    }

    // Recursion to give us chain-lightning -- bwr
    // Low power slight chance added for low power characters -- bwr
    if ((pow >= 10 && !one_chance_in(3)) || (pow >= 3 && one_chance_in(10)))
    {
        mpr("The lightning arcs!");
        pow /= (coinflip() ? 2 : 3);
        damage += apply_random_around_square(_discharge_monsters, where,
                                             true, pow, 1);
    }
    else if (damage > 0)
    {
        // Only printed if we did damage, so that the messages in
        // cast_discharge() are clean. -- bwr
        mpr("The lightning grounds out.");
    }

    return (damage);
}

void cast_discharge(int pow)
{
    int num_targs = 1 + random2(1 + pow / 25);
    int dam;

    dam = apply_random_around_square(_discharge_monsters, you.pos(),
                                     true, pow, num_targs);

    dprf("Arcs: %d Damage: %d", num_targs, dam);

    if (dam == 0)
    {
        if (coinflip())
            mpr("The air around you crackles with electrical energy.");
        else
        {
            const bool plural = coinflip();
            mprf("%s blue arc%s ground%s harmlessly %s you.",
                 plural ? "Some" : "A",
                 plural ? "s" : "",
                 plural ? " themselves" : "s itself",
                 plural ? "around" : (coinflip() ? "beside" :
                                      coinflip() ? "behind" : "before"));
        }
    }
}

// Really this is just applying the best of Band/Warp weapon/Warp field
// into a spell that gives the "make monsters go away" benefit without
// the insane damage potential. - bwr
int disperse_monsters(coord_def where, int pow, int, actor *)
{
    monsters *mon = monster_at(where);
    if (!mon)
        return (0);

    if (mons_genus(mon->type) == MONS_BLINK_FROG)
    {
        simple_monster_message(mon, " resists.");
        return (1);
    }
    else if (mon->check_res_magic(pow))
    {
        // XXX: Note that this might affect magic-immunes!
        if (coinflip())
        {
            simple_monster_message(mon, " partially resists.");
            monster_blink(mon);
        }
        else
            simple_monster_message(mon, " resists.");
        return (1);
    }
    else
    {
        monster_teleport(mon, true);
        return (1);
    }

    return (0);
}

void cast_dispersal(int pow)
{
    if (apply_area_around_square(disperse_monsters, you.pos(), pow) == 0)
        mpr("The air shimmers briefly around you.");
}

bool cast_fragmentation(int pow, const dist& spd)
{
    int debris = 0;
    bool explode = false;
    bool hole    = true;
    const char *what = NULL;

    if (!exists_ray(you.pos(), spd.target))
    {
        mpr("There's a wall in the way!");
        return (false);
    }

    //FIXME: If (player typed '>' to attack floor) goto do_terrain;

    bolt beam;

    beam.flavour     = BEAM_FRAG;
    beam.glyph       = dchar_glyph(DCHAR_FIRED_BURST);
    beam.beam_source = MHITYOU;
    beam.thrower     = KILL_YOU;
    beam.ex_size     = 1;
    beam.source      = you.pos();
    beam.hit         = AUTOMATIC_HIT;

    beam.set_target(spd);
    beam.aux_source.clear();

    // Number of dice vary... 3 is easy/common, but it can get as high as 6.
    beam.damage = dice_def(0, 5 + pow / 10);

    const dungeon_feature_type grid = grd(spd.target);

    if (monsters *mon = monster_at(spd.target))
    {
        // Save the monster's name in case it isn't available later.
        const std::string name_cap_the = mon->name(DESC_CAP_THE);

        switch (mon->type)
        {
        case MONS_WOOD_GOLEM:
            simple_monster_message(mon, " shudders violently!");

            // We use beam.damage not only for inflicting damage here,
            // but so that later on we'll know that the spell didn't
            // fizzle (since we don't actually explode wood golems). - bwr
            explode         = false;
            beam.damage.num = 2;
            _player_hurt_monster(*mon, beam.damage.roll(),
                                 BEAM_DISINTEGRATION);
            break;

        case MONS_IRON_GOLEM:
        case MONS_METAL_GARGOYLE:
            explode         = true;
            beam.name       = "blast of metal fragments";
            beam.colour     = CYAN;
            beam.damage.num = 4;
            if (_player_hurt_monster(*mon, beam.damage.roll(),
                                     BEAM_DISINTEGRATION))
                beam.damage.num += 2;
            break;

        case MONS_CLAY_GOLEM:   // Assume baked clay and not wet loam.
        case MONS_STONE_GOLEM:
        case MONS_EARTH_ELEMENTAL:
        case MONS_GARGOYLE:
        case MONS_STATUE:
            explode         = true;
            beam.ex_size    = 2;
            beam.name       = "blast of rock fragments";
            beam.colour     = BROWN;
            beam.damage.num = 3;
            if (_player_hurt_monster(*mon, beam.damage.roll(),
                                     BEAM_DISINTEGRATION))
                beam.damage.num++;
            break;

        case MONS_SILVER_STATUE:
        case MONS_ORANGE_STATUE:
            explode         = true;
            beam.ex_size    = 2;
            if (mon->type == MONS_SILVER_STATUE)
            {
                beam.name       = "blast of silver fragments";
                beam.colour     = WHITE;
                beam.damage.num = 3;
            }
            else
            {
                beam.name       = "blast of orange crystal shards";
                beam.colour     = LIGHTRED;
                beam.damage.num = 6;
            }

            {
                int statue_damage = beam.damage.roll() * 2;
                if (pow >= 50 && one_chance_in(10))
                    statue_damage = mon->hit_points;

                if (_player_hurt_monster(*mon, statue_damage,
                                         BEAM_DISINTEGRATION))
                    beam.damage.num += 2;
            }
            break;

        case MONS_CRYSTAL_GOLEM:
            explode         = true;
            beam.ex_size    = 2;
            beam.name       = "blast of crystal shards";
            beam.colour     = WHITE;
            beam.damage.num = 4;
            if (_player_hurt_monster(*mon, beam.damage.roll(),
                                     BEAM_DISINTEGRATION))
                beam.damage.num += 2;
            break;

        default:
            if (mon->is_icy()) // blast of ice
            {
                explode         = true;
                beam.name       = "icy blast";
                beam.colour     = WHITE;
                beam.damage.num = 2;
                beam.flavour    = BEAM_ICE;
                if (_player_hurt_monster(*mon, beam.damage.roll()),
                                         BEAM_DISINTEGRATION)
                    beam.damage.num++;
                break;
            }
            else if (mon->is_skeletal()) // blast of bone
            {
                mprf("The %s explodes into sharp fragments of bone!",
                     (mon->type == MONS_FLYING_SKULL) ? "skull" : "skeleton");

                explode     = true;
                beam.name   = "blast of bone shards";
                beam.colour = LIGHTGREY;

                if (x_chance_in_y(pow / 5, 50)) // potential insta-kill
                {
                    monster_die(mon, KILL_YOU, NON_MONSTER);
                    beam.damage.num = 4;
                }
                else
                {
                      beam.damage.num = 2;
                      if (_player_hurt_monster(*mon, beam.damage.roll(),
                                               BEAM_DISINTEGRATION))
                          beam.damage.num += 2;
                  }
                  goto all_done; // i.e., no "Foo Explodes!"
            }
            else
            {
                const bool petrifying = mon->petrifying();
                const bool petrified = mon->petrified() && !petrifying;

                // Petrifying or petrified monsters can be exploded.
                if (petrifying || petrified)
                {
                    explode         = true;
                    beam.ex_size    = petrifying ? 1 : 2;
                    beam.name       = "blast of petrified fragments";
                    beam.colour     = mons_class_colour(mon->type);
                    beam.damage.num = petrifying ? 2 : 3;
                    if (_player_hurt_monster(*mon, beam.damage.roll(),
                                             BEAM_DISINTEGRATION))
                        beam.damage.num++;
                    break;
                }
            }

            // Mark that a monster was targeted.
            beam.damage.num = 1;

            // Yes, this spell does lousy damage if the monster isn't
            // susceptible. - bwr
            _player_hurt_monster(*mon, roll_dice(1, 5 + pow / 25),
                                 BEAM_DISINTEGRATION);
            goto do_terrain;
        }

        mprf("%s shatters!", name_cap_the.c_str());
        goto all_done;
    }

    for (stack_iterator si(spd.target, true); si; ++si)
    {
        if (si->base_type == OBJ_CORPSES)
        {
            std::string nm = si->name(DESC_CAP_THE);
            if (si->sub_type == CORPSE_BODY)
            {
                if (!explode_corpse(*si, spd.target))
                {
                    mprf("%s seems to be exceptionally well connected.",
                         nm.c_str());

                    goto all_done;
                }
            }

            mprf("%s explodes!", nm.c_str());
            destroy_item(si.link());
            // si invalid now!
            goto all_done;
        }
    }

    if (env.markers.property_at(spd.target, MAT_ANY,
                                "veto_fragmentation") == "veto")
    {
        mprf("%s seems to be unnaturally hard.",
             feature_description(spd.target, false, DESC_CAP_THE, false).c_str());
        canned_msg(MSG_SPELL_FIZZLES);
        return (true);
    }

  do_terrain:
    // FIXME: do nothing in Abyss & Pandemonium?

    switch (grid)
    {
    //
    // Stone and rock terrain
    //
    case DNGN_ROCK_WALL:
    case DNGN_CLEAR_ROCK_WALL:
    case DNGN_SECRET_DOOR:
        beam.colour = env.rock_colour;
        // fall-through
    case DNGN_CLEAR_STONE_WALL:
    case DNGN_STONE_WALL:
        what = "wall";
        if (player_in_branch(BRANCH_HALL_OF_ZOT))
            beam.colour = env.rock_colour;
        // fall-through
    case DNGN_ORCISH_IDOL:
        if (what == NULL)
            what = "stone idol";
        if (beam.colour == 0)
            beam.colour = DARKGREY;
        // fall-through
    case DNGN_GRANITE_STATUE:   // normal rock -- big explosion
        if (what == NULL)
            what = "statue";

        explode = true;

        beam.name       = "blast of rock fragments";
        beam.damage.num = 3;
        if (beam.colour == 0)
            beam.colour = LIGHTGREY;

        if ((grid == DNGN_ORCISH_IDOL
             || grid == DNGN_GRANITE_STATUE
             || pow >= 40 && grid == DNGN_ROCK_WALL && one_chance_in(3)
             || pow >= 40 && grid == DNGN_CLEAR_ROCK_WALL
                 && one_chance_in(3)
             || pow >= 60 && grid == DNGN_STONE_WALL && one_chance_in(10)
             || pow >= 60 && grid == DNGN_CLEAR_STONE_WALL
                 && one_chance_in(10)))
        {
            // terrain blew up real good:
            beam.ex_size        = 2;
            grd(spd.target)     = DNGN_FLOOR;
            set_terrain_changed(spd.target);
            debris              = DEBRIS_ROCK;
        }
        break;

    //
    // Metal -- small but nasty explosion
    //

    case DNGN_METAL_WALL:
        what            = "metal wall";
        beam.colour     = CYAN;
        explode         = true;
        beam.name       = "blast of metal fragments";
        beam.damage.num = 4;

        if (pow >= 80 && x_chance_in_y(pow / 5, 500))
        {
            beam.damage.num += 2;
            grd(spd.target)  = DNGN_FLOOR;
            set_terrain_changed(spd.target);
            debris           = DEBRIS_METAL;
        }
        break;

    //
    // Crystal
    //

    case DNGN_GREEN_CRYSTAL_WALL:       // crystal -- large & nasty explosion
        what            = "crystal wall";
        beam.colour     = GREEN;
        explode         = true;
        beam.ex_size    = 2;
        beam.name       = "blast of crystal shards";
        beam.damage.num = 5;

        if (grid == DNGN_GREEN_CRYSTAL_WALL && coinflip())
        {
            beam.ex_size    = coinflip() ? 3 : 2;
            grd(spd.target) = DNGN_FLOOR;
            set_terrain_changed(spd.target);
            debris          = DEBRIS_CRYSTAL;
        }
        break;

    //
    // Traps
    //

    case DNGN_UNDISCOVERED_TRAP:
    case DNGN_TRAP_MECHANICAL:
    {
        trap_def* ptrap = find_trap(spd.target);
        if (ptrap && ptrap->category() != DNGN_TRAP_MECHANICAL)
        {
            // Non-mechanical traps don't explode with this spell. -- bwr
            break;
        }

        // Undiscovered traps appear as exploding from the floor. -- bwr
        what = ((grid == DNGN_UNDISCOVERED_TRAP) ? "floor" : "trap");

        explode         = true;
        hole            = false;    // to hit monsters standing on traps
        beam.name       = "blast of fragments";
        beam.colour     = env.floor_colour;  // in order to blend in
        beam.damage.num = 2;

        // Exploded traps are nonfunctional, ammo is also ruined -- bwr
        ptrap->destroy();
        break;
    }

    //
    // Stone doors and arches
    //

    case DNGN_OPEN_DOOR:
    case DNGN_CLOSED_DOOR:
    case DNGN_DETECTED_SECRET_DOOR:
        // Doors always blow up, stone arches never do (would cause problems).
        grd(spd.target) = DNGN_FLOOR;
        set_terrain_changed(spd.target);

        // fall-through
    case DNGN_STONE_ARCH:          // Floor -- small explosion.
        explode         = true;
        hole            = false;  // to hit monsters standing on doors
        beam.name       = "blast of rock fragments";
        beam.colour     = LIGHTGREY;
        beam.damage.num = 2;
        break;

    //
    // Permarock and floor are unaffected -- bwr
    //
    case DNGN_PERMAROCK_WALL:
    case DNGN_CLEAR_PERMAROCK_WALL:
    case DNGN_FLOOR:
        explode = false;
        mprf("%s seems to be unnaturally hard.",
             (grid == DNGN_FLOOR) ? "The dungeon floor"
                                  : "That wall");
        break;

    default:
        // FIXME: cute message for water?
        break;
    }

  all_done:
    if (explode && beam.damage.num > 0)
    {
        if (what != NULL)
            mprf("The %s shatters!", what);

        beam.explode(true, hole);

        if (grid == DNGN_ORCISH_IDOL)
            did_god_conduct(DID_DESTROY_ORCISH_IDOL, 8);
    }
    else if (beam.damage.num == 0)
    {
        // If damage dice are zero, assume that nothing happened at all.
        canned_msg(MSG_SPELL_FIZZLES);
    }

    return (true);
}

bool wielding_rocks()
{
    bool rc = false;
    if (you.weapon())
    {
        const item_def& wpn(*you.weapon());
        rc = (wpn.base_type == OBJ_MISSILES
              && (wpn.sub_type == MI_STONE || wpn.sub_type == MI_LARGE_ROCK));
    }
    return (rc);
}

bool cast_sandblast(int pow, bolt &beam)
{
    const bool big     = wielding_rocks();
    const bool success = zapping(big ? ZAP_SANDBLAST
                                     : ZAP_SMALL_SANDBLAST, pow, beam, true);

    if (big && success)
        dec_inv_item_quantity( you.equip[EQ_WEAPON], 1 );

    return (success);
}
