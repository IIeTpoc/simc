// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"
#include "util/util.hpp"
#include "class_modules/apl/apl_rogue.hpp"

namespace { // UNNAMED NAMESPACE

// Forward Declarations
class rogue_t;

constexpr double COMBO_POINT_MAX = 5;

enum class secondary_trigger
{
  NONE = 0U,
  SINISTER_STRIKE,
  WEAPONMASTER,
  SECRET_TECHNIQUE,
  SECRET_TECHNIQUE_CLONE,
  SHURIKEN_TORNADO,
  INTERNAL_BLEEDING,
  TRIPLE_THREAT,
  FLAGELLATION,
  MAIN_GAUCHE,
  DEATHMARK,
  VICIOUS_VENOMS,
  HIDDEN_OPPORTUNITY,
  FAN_THE_HAMMER,
  CRACKSHOT,
  COUP_DE_GRACE,
  HAND_OF_FATE,
};

enum stealth_type_e
{
  STEALTH_NORMAL = 0x01,
  STEALTH_VANISH = 0x02,
  STEALTH_SHADOWMELD = 0x04,
  STEALTH_SUBTERFUGE = 0x08,
  STEALTH_SHADOW_DANCE = 0x10,
  STEALTH_IMPROVED_GARROTE = 0x20,

  STEALTH_BASIC = ( STEALTH_NORMAL | STEALTH_VANISH ),            // Normal + Vanish
  STEALTH_ROGUE = ( STEALTH_SUBTERFUGE | STEALTH_SHADOW_DANCE ),  // Subterfuge + Shadowdance

  // All Stealth states that enable Stealth ability stance masks
  STEALTH_STANCE = ( STEALTH_BASIC | STEALTH_ROGUE | STEALTH_SHADOWMELD ),

  STEALTH_ALL = 0xFF
};

struct fatebound_t
{
  enum coinflip_e { HEADS, TAILS, EDGE };
};

namespace actions
{
  struct rogue_attack_t;
  struct rogue_heal_t;
  struct rogue_spell_t;

  struct rogue_poison_t;
  struct melee_t;
  struct shadow_blades_attack_t;
  struct flagellation_damage_t;
}

enum current_weapon_e
{
  WEAPON_PRIMARY = 0U,
  WEAPON_SECONDARY
};

enum weapon_slot_e
{
  WEAPON_MAIN_HAND = 0U,
  WEAPON_OFF_HAND
};

struct weapon_info_t
{
  // State of the hand, i.e., primary or secondary weapon currently equipped
  current_weapon_e     current_weapon;
  // Pointers to item data
  const item_t*        item_data[ 2 ];
  // Computed weapon data
  weapon_t             weapon_data[ 2 ];
  // Computed stats data
  gear_stats_t         stats_data[ 2 ];
  // Callbacks, associated with special effects on each weapons
  std::vector<dbc_proc_callback_t*> cb_data[ 2 ];

  // Item data storage for secondary weapons
  item_t               secondary_weapon_data;

  // Protect against multiple initialization since the init is done in an action_t object.
  bool                 initialized;

  // Track secondary weapon uptime through a buff
  buff_t*              secondary_weapon_uptime;

  weapon_info_t() :
    current_weapon( WEAPON_PRIMARY ), initialized( false ), secondary_weapon_uptime( nullptr )
  {
    range::fill( item_data, nullptr );
  }

  weapon_slot_e slot() const;
  void initialize();
  void reset();

  // Enable/disable callbacks on the primary/secondary weapons.
  void callback_state( current_weapon_e weapon, bool state );
};

namespace buffs {
  struct rogue_buff_t : public buff_t
  {
    rogue_buff_t( player_t* p , util::string_view name, const spell_data_t* spell_data ) :
      buff_t( p->sim, p, p, name, spell_data, nullptr )
    {}

    // Used by underhanded upper hand, it's not a *real* pause, but rather an application of a 100x slowdown via time mod
    rogue_buff_t* pause()
    {
      set_dynamic_time_duration_multiplier( 100.0 );
      return this;
    }

    rogue_buff_t* unpause()
    {
      set_dynamic_time_duration_multiplier( 1.0 );
      return this;
    }
  };
}

// ==========================================================================
// Rogue Target Data
// ==========================================================================

class rogue_td_t : public actor_target_data_t
{
  std::vector<dot_t*> bleeds;
  std::vector<dot_t*> poison_dots;
  std::vector<buff_t*> poison_debuffs;

public:
  struct dots_t
  {
    dot_t* crimson_tempest;
    dot_t* deadly_poison;
    dot_t* deadly_poison_deathmark;
    dot_t* deathmark;
    dot_t* garrote;
    dot_t* garrote_deathmark;
    dot_t* internal_bleeding;
    dot_t* killing_spree; // Strictly speaking, this should probably be on player
    dot_t* kingsbane;
    dot_t* mutilated_flesh;
    dot_t* rupture;
    dot_t* rupture_deathmark;
    dot_t* serrated_bone_spike;
    dot_t* soulrip;
  } dots;

  struct debuffs_t
  {
    buff_t* amplifying_poison;
    buff_t* amplifying_poison_deathmark;
    buff_t* atrophic_poison;
    buff_t* between_the_eyes;
    buff_t* caustic_spatter;
    buff_t* corrupt_the_blood;
    buff_t* crippling_poison;
    damage_buff_t* deathmark;
    buff_t* deathstalkers_mark;
    buff_t* fatal_intent;
    damage_buff_t* fazed;
    buff_t* find_weakness;
    buff_t* flagellation;
    buff_t* ghostly_strike;
    buff_t* numbing_poison;
    buff_t* sting_like_a_bee;
    damage_buff_t* shiv;
    buff_t* wound_poison;
  } debuffs;

  rogue_td_t( player_t* target, rogue_t* source );

  timespan_t lethal_poison_remains() const
  {
    if ( dots.deadly_poison->is_ticking() )
      return dots.deadly_poison->remains();

    if ( dots.deadly_poison_deathmark->is_ticking() )
      return dots.deadly_poison_deathmark->remains();

    if ( debuffs.wound_poison->check() )
      return debuffs.wound_poison->remains();

    if ( debuffs.amplifying_poison->check() )
      return debuffs.amplifying_poison->remains();

    if ( debuffs.amplifying_poison_deathmark->check() )
      return debuffs.amplifying_poison_deathmark->remains();

    return 0_s;
  }

  timespan_t non_lethal_poison_remains() const
  {
    if ( debuffs.atrophic_poison->check() )
      return debuffs.atrophic_poison->remains();

    if ( debuffs.crippling_poison->check() )
      return debuffs.crippling_poison->remains();

    if ( debuffs.numbing_poison->check() )
      return debuffs.numbing_poison->remains();

    return 0_s;
  }

  bool is_lethal_poisoned() const
  {
    return dots.deadly_poison->is_ticking() || dots.deadly_poison_deathmark->is_ticking() || 
           debuffs.amplifying_poison->check() || debuffs.amplifying_poison_deathmark->check() ||
           debuffs.wound_poison->check();
  }

  bool is_non_lethal_poisoned() const
  {
    return debuffs.atrophic_poison->check() || debuffs.crippling_poison->check() || debuffs.numbing_poison->check();
  }

  bool is_poisoned() const
  {
    return is_lethal_poisoned() || is_non_lethal_poisoned();
  }

  bool is_bleeding() const
  {
    for ( auto b : bleeds )
    {
      if ( b->is_ticking() )
        return true;
    }
    return false;
  }

  int total_bleeds() const
  {
    // TOCHECK -- Confirm all these things count as intended
    return as<int>( range::count_if( bleeds, []( dot_t* dot ) { return dot->is_ticking(); } ) );
  }

  int total_poisons() const
  {
    // TOCHECK -- Confirm all these things count as intended
    return as<int>( range::count_if( poison_dots, []( dot_t* d ) { return d->is_ticking(); } ) +
                    range::count_if( poison_debuffs, []( buff_t* b ) { return b->check(); } ) );
  }

  // Total count of active DoT effects that contribute towards the Lethal Dose talent
  int lethal_dose_count() const
  {
    return total_bleeds() + total_poisons();
  }
};

// ==========================================================================
// Rogue
// ==========================================================================

class rogue_t : public player_t
{
public:
  // Shadow Techniques swing counter;
  unsigned shadow_techniques_counter;

  // Danse Macabre Ability ID Tracker
  std::vector<unsigned> danse_macabre_tracker;

  buff_t* deathstalkers_mark_debuff;

  // Active
  struct
  {
    actions::rogue_poison_t* lethal_poison = nullptr;
    actions::rogue_poison_t* lethal_poison_dtb = nullptr;
    actions::rogue_poison_t* nonlethal_poison = nullptr;
    actions::rogue_poison_t* nonlethal_poison_dtb = nullptr;
    actions::rogue_attack_t* blade_flurry = nullptr;
    actions::rogue_attack_t* caustic_spatter = nullptr;
    actions::rogue_attack_t* echoing_reprimand = nullptr;
    actions::rogue_attack_t* fan_the_hammer = nullptr;
    actions::flagellation_damage_t* flagellation = nullptr;
    actions::rogue_attack_t* internal_bleeding = nullptr;
    actions::rogue_attack_t* lingering_shadow = nullptr;
    actions::rogue_attack_t* main_gauche = nullptr;
    actions::rogue_attack_t* poison_bomb = nullptr;
    actions::rogue_attack_t* serrated_bone_spike = nullptr;
    actions::shadow_blades_attack_t* shadow_blades_attack = nullptr;
    actions::rogue_spell_t* thistle_tea = nullptr;
    actions::rogue_attack_t* triple_threat_mh = nullptr;
    actions::rogue_attack_t* triple_threat_oh = nullptr;

    residual_action::residual_periodic_action_t<spell_t>* doomblade = nullptr;

    struct
    {
      actions::rogue_attack_t* backstab = nullptr;
      actions::rogue_attack_t* gloomblade = nullptr;
      actions::rogue_attack_t* shadowstrike = nullptr;
    } weaponmaster;
    struct
    {
      actions::rogue_attack_t* amplifying_poison = nullptr;
      actions::rogue_attack_t* deadly_poison_dot = nullptr;
      actions::rogue_attack_t* deadly_poison_instant = nullptr;
      actions::rogue_attack_t* garrote = nullptr;
      actions::rogue_attack_t* instant_poison = nullptr;
      actions::rogue_attack_t* rupture = nullptr;
      actions::rogue_attack_t* wound_poison = nullptr;
    } deathmark;
    struct
    {
      actions::rogue_attack_t* mutilate_mh = nullptr;
      actions::rogue_attack_t* mutilate_oh = nullptr;
      actions::rogue_attack_t* ambush = nullptr;
    } vicious_venoms;
    struct
    {
      actions::rogue_attack_t* crimson_tempest = nullptr;
      actions::rogue_attack_t* garrote = nullptr;
      actions::rogue_attack_t* rupture = nullptr;
      actions::rogue_attack_t* deathmark_garrote = nullptr;
      actions::rogue_attack_t* deathmark_rupture = nullptr;
    } sanguine_blades;
    struct
    {
      actions::rogue_attack_t* clear_the_witnesses = nullptr;
      actions::rogue_attack_t* clear_the_witnesses_tornado = nullptr;
      actions::rogue_attack_t* corrupt_the_blood = nullptr;
      actions::rogue_attack_t* deathstalkers_mark = nullptr;
      actions::rogue_attack_t* fatal_intent = nullptr;
      actions::rogue_attack_t* hunt_them_down = nullptr;
      actions::rogue_attack_t* singular_focus = nullptr;
    } deathstalker;
    struct
    {
      actions::rogue_attack_t* fatebound_coin_tails = nullptr;
      actions::rogue_attack_t* fate_intertwined = nullptr;
      actions::rogue_attack_t* lucky_coin = nullptr;
    } fatebound;
    struct
    {
      actions::rogue_attack_t* nimble_flurry = nullptr;
      actions::rogue_attack_t* unseen_blade = nullptr;
    } trickster;
    
    struct
    {
      actions::rogue_attack_t* ethereal_rampage = nullptr;
    } tww1;
  } active;

  // Autoattacks
  action_t* auto_attack;
  actions::melee_t* melee_main_hand;
  actions::melee_t* melee_off_hand;

  // Is using stealth during combat allowed? Relevant for Dungeon sims.
  bool restealth_allowed;

  // Experimental weapon swapping
  std::array<weapon_info_t, 2> weapon_data;

  // Buffs
  struct buffs_t
  {
    // Baseline
    // Shared
    buff_t* feint;
    buff_t* shadowstep;
    buff_t* sprint;
    buff_t* stealth;
    buff_t* vanish;
    // Assassination
    buff_t* envenom;
    // Outlaw
    buffs::rogue_buff_t* adrenaline_rush;
    buff_t* between_the_eyes;
    buffs::rogue_buff_t* blade_flurry;
    buff_t* blade_rush;
    buff_t* opportunity;
    buff_t* roll_the_bones;
    // Roll the bones buffs
    damage_buff_t* broadside;
    buff_t* buried_treasure;
    damage_buff_t* grand_melee;
    buff_t* skull_and_crossbones;
    damage_buff_t* ruthless_precision;
    buff_t* true_bearing;
    // Subtlety
    buff_t* shadow_blades;
    damage_buff_t* shadow_dance;
    damage_buff_t* symbols_of_death;

    // Talents
    // Shared
    std::vector<buff_t*> supercharger;

    damage_buff_t* acrobatic_strikes;
    buff_t* alacrity;
    damage_buff_t* cold_blood;
    buff_t* subterfuge;
    buff_t* thistle_tea;
    buff_t* echoing_reprimand;

    // Hero
    // Deathstalker
    buff_t* clear_the_witnesses;
    damage_buff_t* deathstalkers_mark;
    buff_t* darkest_night;
    buff_t* lingering_darkness;
    damage_buff_t* momentum_of_despair;
    damage_buff_t* symbolic_victory;

    // Fatebound
    damage_buff_t* fatebound_coin_heads;
    buff_t* fatebound_coin_tails;
    stat_buff_t* fatebound_lucky_coin;
    buff_t* double_jeopardy; // not a buff in-game, but useful to track as a buff

    // Trickster
    buff_t* cloud_cover;
    buff_t* disorienting_strikes;
    buff_t* escalating_blade;
    damage_buff_t* flawless_form;
    buff_t* unseen_blade_cd;
    
    // Assassination
    buff_t* blindside;
    buff_t* improved_garrote;
    buff_t* improved_garrote_aura;
    buff_t* indiscriminate_carnage;
    buff_t* indiscriminate_carnage_aura;
    damage_buff_t* kingsbane;
    damage_buff_t* master_assassin;
    damage_buff_t* master_assassin_aura;
    buff_t* serrated_bone_spike_charges;
    buff_t* scent_of_blood;

    // Outlaw
    buff_t* audacity;
    buff_t* greenskins_wickers;
    buff_t* killing_spree;
    buff_t* loaded_dice;
    buffs::rogue_buff_t* slice_and_dice;
    buff_t* take_em_by_surprise;
    buff_t* take_em_by_surprise_aura;
    damage_buff_t* summarily_dispatched;

    // Subtlety
    damage_buff_t* danse_macabre;
    buff_t* deeper_daggers;
    damage_buff_t* finality_eviscerate;
    buff_t* finality_rupture;
    damage_buff_t* finality_black_powder;
    buff_t* flagellation;
    buff_t* flagellation_persist;
    buff_t* goremaws_bite;
    buff_t* lingering_shadow;
    buff_t* master_of_shadows;
    damage_buff_t* perforated_veins;
    buff_t* perforated_veins_counter;
    buff_t* premeditation;
    buff_t* secret_technique;   // Only to simplify APL tracking
    buff_t* shadow_techniques;  // Internal tracking buff
    buff_t* shot_in_the_dark;
    buff_t* shuriken_tornado;
    buff_t* silent_storm;
    buff_t* the_first_dance;
    damage_buff_t* the_rotten;

    // Set Bonuses
    damage_buff_t* tww1_assassination_2pc;
    damage_buff_t* tww1_assassination_4pc;
    damage_buff_t* tww1_outlaw_4pc;
    damage_buff_t* tww1_subtlety_2pc;
    damage_buff_t* tww1_subtlety_4pc;

    damage_buff_t* tww2_assassination_2pc;
    damage_buff_t* tww2_assassination_4pc;
    damage_buff_t* tww2_outlaw_2pc;
    damage_buff_t* tww2_subtlety_2pc;

  } buffs;

  // Cooldowns
  struct cooldowns_t
  {
    cooldown_t* stealth;

    cooldown_t* adrenaline_rush;
    cooldown_t* between_the_eyes;
    cooldown_t* blade_flurry;
    cooldown_t* blade_rush;
    cooldown_t* blind;
    cooldown_t* cloak_of_shadows;
    cooldown_t* cold_blood;
    cooldown_t* deathmark;
    cooldown_t* evasion;
    cooldown_t* feint;
    cooldown_t* flagellation;
    cooldown_t* garrote;
    cooldown_t* ghostly_strike;
    cooldown_t* goremaws_bite;
    cooldown_t* gouge;
    cooldown_t* grappling_hook;
    cooldown_t* keep_it_rolling;
    cooldown_t* killing_spree;
    cooldown_t* kingsbane;
    cooldown_t* roll_the_bones;
    cooldown_t* secret_technique;
    cooldown_t* serrated_bone_spike;
    cooldown_t* shadow_blades;
    cooldown_t* shadow_dance;
    cooldown_t* shadowstep;
    cooldown_t* shiv;
    cooldown_t* shuriken_tornado;
    cooldown_t* sprint;
    cooldown_t* symbols_of_death;
    cooldown_t* thistle_tea;
    cooldown_t* unseen_blade_icd;
    cooldown_t* vanish;

    target_specific_cooldown_t* weaponmaster;
  } cooldowns;

  // Gains
  struct gains_t
  {
    gain_t* adrenaline_rush;
    gain_t* adrenaline_rush_expiry;
    gain_t* blade_rush;
    gain_t* buried_treasure;
    gain_t* darkest_night;
    gain_t* dashing_scoundrel;
    gain_t* fatal_flourish;
    gain_t* energy_refund;
    gain_t* master_of_shadows;
    gain_t* venomous_wounds;
    gain_t* venomous_wounds_death;
    gain_t* relentless_strikes;
    gain_t* symbols_of_death;
    gain_t* slice_and_dice;

    // CP Gains
    gain_t* ace_up_your_sleeve;
    gain_t* broadside;
    gain_t* improved_adrenaline_rush;
    gain_t* improved_adrenaline_rush_expiry;
    gain_t* improved_ambush;
    gain_t* premeditation;
    gain_t* quick_draw;
    gain_t* ruthlessness;
    gain_t* seal_fate;
    gain_t* serrated_bone_spike;
    gain_t* shadow_techniques;
    gain_t* shadow_techniques_shadowcraft;
    gain_t* shadow_blades;
    gain_t* shrouded_suffocation;
    gain_t* deal_fate;

  } gains;

  // Spell Data
  struct spells_t
  {
    // Core Class Spells
    const spell_data_t* ambush;
    const spell_data_t* cheap_shot;
    const spell_data_t* crimson_vial;           // No implementation
    const spell_data_t* crippling_poison;
    const spell_data_t* detection;
    const spell_data_t* distract;
    const spell_data_t* echoing_reprimand_buff;
    const spell_data_t* echoing_reprimand_damage;
    const spell_data_t* instant_poison;
    const spell_data_t* kick;
    const spell_data_t* kidney_shot;
    const spell_data_t* shadowstep;
    const spell_data_t* slice_and_dice;
    const spell_data_t* sprint;
    const spell_data_t* stealth;
    const spell_data_t* thistle_tea;
    const spell_data_t* vanish;
    const spell_data_t* wound_poison;
    const spell_data_t* feint;
    const spell_data_t* sap;
    const spell_data_t* cold_blood;

    // Class Passives
    const spell_data_t* all_rogue;
    const spell_data_t* critical_strikes;
    const spell_data_t* cut_to_the_chase;
    const spell_data_t* fleet_footed;           // DFALPHA: Duplicate passive?
    const spell_data_t* leather_specialization;

    // Background Spells
    const spell_data_t* acrobatic_strikes_buff;
    const spell_data_t* alacrity_buff;
    const spell_data_t* leeching_poison_buff;
    const spell_data_t* recuperator_heal;
    const spell_data_t* shadowstep_buff;
    const spell_data_t* subterfuge_buff;
    const spell_data_t* thistle_tea_buff;
    const spell_data_t* vanish_buff;

    // Hero Spells
    const spell_data_t* clear_the_witnesses_buff;
    const spell_data_t* clear_the_witnesses_damage;
    const spell_data_t* cloud_cover_distract;
    const spell_data_t* corrupt_the_blood_damage;
    const spell_data_t* coup_de_grace;
    const spell_data_t* coup_de_grace_damage_1;
    const spell_data_t* coup_de_grace_damage_2;
    const spell_data_t* coup_de_grace_damage_3;
    const spell_data_t* coup_de_grace_damage_bonus_1;
    const spell_data_t* coup_de_grace_damage_bonus_2;
    const spell_data_t* coup_de_grace_damage_bonus_3;
    const spell_data_t* darkest_night_buff;
    const spell_data_t* deathstalkers_mark_buff;
    const spell_data_t* deathstalkers_mark_damage;
    const spell_data_t* deathstalkers_mark_debuff;
    const spell_data_t* escalating_blade_buff;
    const spell_data_t* fatal_intent_damage;
    const spell_data_t* fatal_intent_debuff;
    const spell_data_t* fatebound_coin_heads_buff;
    const spell_data_t* fatebound_coin_heads_stacking_buff;
    const spell_data_t* fatebound_coin_tails_buff;
    const spell_data_t* fatebound_coin_tails;
    const spell_data_t* fatebound_lucky_coin_buff;
    const spell_data_t* fatebound_lucky_coin_damage;
    const spell_data_t* fatebound_fate_intertwined;
    const spell_data_t* fazed_debuff;
    const spell_data_t* flawless_form_buff;
    const spell_data_t* hunt_them_down_damage;
    const spell_data_t* lingering_darkness_buff;
    const spell_data_t* momentum_of_despair_buff;
    const spell_data_t* nimble_flurry_damage;
    const spell_data_t* singular_focus_damage;
    const spell_data_t* symbolic_victory_buff;
    const spell_data_t* unseen_blade;
    const spell_data_t* unseen_blade_buff;

  } spell;

  // Spec Spell Data
  struct spec_t
  {
    // Assassination Spells
    const spell_data_t* assassination_rogue;
    const spell_data_t* envenom;
    const spell_data_t* fan_of_knives;
    const spell_data_t* garrote;
    const spell_data_t* mutilate;
    const spell_data_t* poisoned_knife;

    const spell_data_t* amplifying_poison_debuff;
    const spell_data_t* blindside_buff;
    const spell_data_t* caustic_spatter_buff;
    const spell_data_t* caustic_spatter_damage;
    const spell_data_t* dashing_scoundrel;
    double dashing_scoundrel_gain;
    const spell_data_t* deadly_poison_instant;
    const spell_data_t* improved_garrote_buff;
    const spell_data_t* improved_shiv_debuff;
    const spell_data_t* indiscriminate_carnage_buff;
    const spell_data_t* indiscriminate_carnage_buff_aura;
    const spell_data_t* internal_bleeding_debuff;
    const spell_data_t* kingsbane_buff;
    const spell_data_t* master_assassin_aura_buff;
    const spell_data_t* master_assassin_buff;
    const spell_data_t* poison_bomb_driver;
    const spell_data_t* poison_bomb_damage;
    const spell_data_t* sanguine_blades_damage;
    const spell_data_t* serrated_bone_spike_buff;
    const spell_data_t* serrated_bone_spike_damage;
    const spell_data_t* serrated_bone_spike_energize;
    const spell_data_t* scent_of_blood_buff;
    const spell_data_t* vicious_venoms_ambush;
    const spell_data_t* vicious_venoms_mutilate_mh;
    const spell_data_t* vicious_venoms_mutilate_oh;
    const spell_data_t* zoldyck_insignia;

    const spell_data_t* deathmark_debuff;
    const spell_data_t* deathmark_amplifying_poison;
    const spell_data_t* deathmark_deadly_poison_dot;
    const spell_data_t* deathmark_deadly_poison_instant;
    const spell_data_t* deathmark_garrote;
    const spell_data_t* deathmark_instant_poison;
    const spell_data_t* deathmark_rupture;
    const spell_data_t* deathmark_wound_poison;

    // Outlaw Spells
    const spell_data_t* outlaw_rogue;
    const spell_data_t* between_the_eyes;
    const spell_data_t* dispatch;
    const spell_data_t* pistol_shot;
    const spell_data_t* sinister_strike;
    const spell_data_t* roll_the_bones;
    const spell_data_t* restless_blades;
    const spell_data_t* blade_flurry;

    const spell_data_t* audacity_buff;
    const spell_data_t* blade_flurry_attack;
    const spell_data_t* blade_flurry_instant_attack;
    const spell_data_t* blade_rush_attack;
    const spell_data_t* blade_rush_energize;
    const spell_data_t* doomblade_debuff;
    const spell_data_t* greenskins_wickers;
    const spell_data_t* greenskins_wickers_buff;
    const spell_data_t* hidden_opportunity_extra_attack;
    const spell_data_t* improved_adrenaline_rush_energize;
    const spell_data_t* killing_spree_mh_attack;
    const spell_data_t* killing_spree_oh_attack;
    const spell_data_t* opportunity_buff;
    const spell_data_t* sinister_strike_extra_attack;
    const spell_data_t* sting_like_a_bee_debuff;
    const spell_data_t* summarily_dispatched_buff;
    const spell_data_t* take_em_by_surprise_buff;
    const spell_data_t* triple_threat_attack;
    const spell_data_t* ace_up_your_sleeve_energize;

    const spell_data_t* broadside;
    const spell_data_t* buried_treasure;
    const spell_data_t* grand_melee;
    const spell_data_t* skull_and_crossbones;
    const spell_data_t* ruthless_precision;
    const spell_data_t* true_bearing;

    // Subtlety Spells
    const spell_data_t* subtlety_rogue;
    const spell_data_t* backstab;
    const spell_data_t* black_powder;
    const spell_data_t* eviscerate;
    const spell_data_t* shadow_dance;
    const spell_data_t* shadow_techniques;
    const spell_data_t* shadowstrike;
    const spell_data_t* shuriken_storm;    
    const spell_data_t* shuriken_toss;
    const spell_data_t* symbols_of_death;

    const spell_data_t* black_powder_shadow_attack;
    const spell_data_t* danse_macabre_buff;
    const spell_data_t* deeper_daggers_buff;
    const spell_data_t* eviscerate_shadow_attack;
    const spell_data_t* find_weakness_debuff;
    const spell_data_t* finality_black_powder_buff;
    const spell_data_t* finality_eviscerate_buff;
    const spell_data_t* finality_rupture_buff;
    const spell_data_t* flagellation_buff;
    const spell_data_t* flagellation_persist_buff;
    const spell_data_t* flagellation_damage;
    const spell_data_t* goremaws_bite_buff;
    const spell_data_t* master_of_shadows_buff;
    const spell_data_t* perforated_veins_buff;
    const spell_data_t* perforated_veins_counter;
    const spell_data_t* premeditation_buff;
    const spell_data_t* relentless_strikes_energize;
    const spell_data_t* replicating_shadows_tick;
    const spell_data_t* lingering_shadow_attack;
    const spell_data_t* lingering_shadow_buff;
    const spell_data_t* secret_technique_attack;
    const spell_data_t* secret_technique_clone_attack;
    const spell_data_t* shadow_blades_attack;
    const spell_data_t* shadow_focus_buff;
    const spell_data_t* shadow_techniques_energize;
    const spell_data_t* shadowstrike_stealth_buff;
    const spell_data_t* shot_in_the_dark_buff;
    const spell_data_t* silent_storm_buff;
    const spell_data_t* the_first_dance_buff;

    // Multi-Spec
    const spell_data_t* rupture;      // Assassination + Subtlety
    const spell_data_t* shadowstep;   // Assassination + Subtlety, baseline charge increase passive

    // Set Bonuses
    const spell_data_t* tww1_assassination_2pc_buff;
    const spell_data_t* tww1_assassination_4pc_buff;
    const spell_data_t* tww1_outlaw_2pc_spell;
    const spell_data_t* tww1_outlaw_4pc_buff;
    const spell_data_t* tww2_assassination_4pc_buff;

  } spec;

  // Talents
  struct talents_t
  {
    struct class_talents_t
    {
      player_talent_t shiv;
      player_talent_t blind;                    // No implementation
      player_talent_t cloak_of_shadows;         // No implementation

      player_talent_t evasion;                  // No implementation
      player_talent_t gouge;
      player_talent_t airborne_irritant;        // No implementation
      player_talent_t thrill_seeking;

      player_talent_t master_poisoner;          // No implementation
      player_talent_t elusiveness;              // No implementation
      player_talent_t cheat_death;              // No implementation
      player_talent_t blackjack;                // No implementation
      player_talent_t tricks_of_the_trade;      // No implementation

      player_talent_t improved_wound_poison;
      player_talent_t nimble_fingers;
      player_talent_t improved_sprint;
      player_talent_t shadowrunner;
      
      player_talent_t superior_mixture;         // No implementation
      player_talent_t fleet_footed;             // No implementation
      player_talent_t iron_stomach;             // No implementation
      player_talent_t unbreakable_stride;       // No implementation
      player_talent_t featherfoot;
      player_talent_t rushed_setup;
      player_talent_t shadowheart;              // NYI

      player_talent_t numbing_poison;
      player_talent_t atrophic_poison;
      player_talent_t deadened_nerves;          // No implementation
      player_talent_t graceful_guile;           // No implementation
      player_talent_t stillshroud;              // No implementation

      player_talent_t deadly_precision;
      player_talent_t virulent_poisons;
      player_talent_t acrobatic_strikes;
      player_talent_t improved_ambush;
      player_talent_t tight_spender;

      player_talent_t leeching_poison;
      player_talent_t lethality;
      player_talent_t recuperator;
      player_talent_t alacrity;
      player_talent_t soothing_darkness;        // No implementation

      player_talent_t vigor;
      player_talent_t supercharger;
      player_talent_t subterfuge;

      player_talent_t thistle_tea;
      player_talent_t cold_blood;
      player_talent_t echoing_reprimand;
      player_talent_t forced_induction;
      player_talent_t deeper_stratagem;
      player_talent_t without_a_trace;
    } rogue;

    struct assassination_talents_t
    {
      player_talent_t deadly_poison;

      player_talent_t improved_shiv;
      player_talent_t venomous_wounds;
      player_talent_t path_of_blood;

      player_talent_t rapid_injection;
      player_talent_t improved_poisons;
      player_talent_t bloody_mess;

      player_talent_t thrown_precision;
      player_talent_t seal_fate;
      player_talent_t caustic_spatter;
      player_talent_t internal_bleeding;
      player_talent_t improved_garrote;

      player_talent_t crimson_tempest;
      player_talent_t lightweight_shiv;
      player_talent_t deathmark;
      player_talent_t sanguine_blades;
      player_talent_t master_assassin;

      player_talent_t flying_daggers;
      player_talent_t sanguine_stratagem;
      player_talent_t vicious_venoms;
      player_talent_t fatal_concoction;
      player_talent_t lethal_dose;
      player_talent_t intent_to_kill;
      player_talent_t iron_wire;                // No implementation

      player_talent_t systemic_failure;
      player_talent_t amplifying_poison;
      player_talent_t twist_the_knife;
      player_talent_t doomblade;

      player_talent_t blindside;
      player_talent_t tiny_toxic_blade;
      player_talent_t dashing_scoundrel;
      player_talent_t shrouded_suffocation;
      player_talent_t serrated_bone_spike;

      player_talent_t zoldyck_recipe;
      player_talent_t poison_bomb;
      player_talent_t scent_of_blood;

      player_talent_t arterial_precision;
      player_talent_t kingsbane;
      player_talent_t dragon_tempered_blades;
      player_talent_t indiscriminate_carnage;
      player_talent_t sudden_demise;            // Partial NYI for "execute" mechanic
      
    } assassination;

    struct outlaw_talents_t
    {
      player_talent_t opportunity;

      player_talent_t adrenaline_rush;

      player_talent_t retractable_hook;
      player_talent_t dirty_tricks;
      player_talent_t combat_potency;
      player_talent_t combat_stamina;
      player_talent_t hit_and_run;

      player_talent_t blinding_powder;
      player_talent_t float_like_a_butterfly;
      player_talent_t sting_like_a_bee;
      player_talent_t riposte;                  // No implementation
      player_talent_t precision_shot;

      player_talent_t heavy_hitter;
      player_talent_t devious_stratagem;
      player_talent_t killing_spree;
      player_talent_t fatal_flourish;
      player_talent_t ambidexterity;
      player_talent_t quick_draw;
      player_talent_t deft_maneuvers;

      player_talent_t ruthlessness;
      player_talent_t swift_slasher;
      player_talent_t loaded_dice;
      player_talent_t sleight_of_hand;
      player_talent_t thiefs_versatility;
      player_talent_t improved_between_the_eyes;

      player_talent_t audacity;
      player_talent_t triple_threat;
      player_talent_t improved_adrenaline_rush;
      player_talent_t improved_main_gauche;
      player_talent_t dancing_steel;

      player_talent_t underhanded_upper_hand;
      player_talent_t count_the_odds;
      player_talent_t ace_up_your_sleeve;
      player_talent_t blade_rush;
      player_talent_t precise_cuts;

      player_talent_t take_em_by_surprise;
      player_talent_t summarily_dispatched;
      player_talent_t fan_the_hammer;

      player_talent_t hidden_opportunity;
      player_talent_t crackshot;
      player_talent_t keep_it_rolling;
      player_talent_t ghostly_strike;
      player_talent_t greenskins_wickers;

    } outlaw;

    struct subtlety_talents_t
    {
      player_talent_t find_weakness;

      player_talent_t improved_backstab;
      player_talent_t shadow_blades;
      player_talent_t improved_shuriken_storm;

      player_talent_t shot_in_the_dark;
      player_talent_t quick_decisions;
      player_talent_t ephemeral_bond;           // No implementation
      player_talent_t exhilarating_execution;   // No implementation

      player_talent_t shrouded_in_darkness;     // No implementation
      player_talent_t shadow_focus;
      player_talent_t fade_to_nothing;          // No implementation
      player_talent_t cloaked_in_shadow;        // No implementation
      player_talent_t night_terrors;            // No implementation
      player_talent_t terrifying_pace;          // No implementation

      player_talent_t swift_death;
      player_talent_t improved_shadow_techniques;
      player_talent_t gloomblade;
      player_talent_t improved_shadow_dance;
      player_talent_t secret_technique;
      player_talent_t relentless_strikes;
      player_talent_t silent_storm;

      player_talent_t premeditation;
      player_talent_t planned_execution;
      player_talent_t warning_signs;
      player_talent_t double_dance;
      player_talent_t shadowed_finishers;
      player_talent_t secret_stratagem;
      player_talent_t replicating_shadows;

      player_talent_t weaponmaster;
      player_talent_t the_first_dance;
      player_talent_t master_of_shadows;
      player_talent_t deepening_shadows;
      player_talent_t veiltouched;
      player_talent_t shuriken_tornado;

      player_talent_t inevitability;     
      player_talent_t perforated_veins;
      player_talent_t lingering_shadow;
      player_talent_t deeper_daggers;
      player_talent_t flagellation;

      player_talent_t death_perception;
      player_talent_t dark_shadow;
      player_talent_t finality;

      player_talent_t the_rotten;
      player_talent_t shadowcraft;              // Partial NYI, TODO finisher interaction
      player_talent_t danse_macabre;
      player_talent_t goremaws_bite;
      player_talent_t dark_brew;

    } subtlety;

    struct deathstalker_talents_t
    {
      player_talent_t deathstalkers_mark;

      player_talent_t clear_the_witnesses;
      player_talent_t hunt_them_down;
      player_talent_t singular_focus;

      player_talent_t fatal_intent;
      player_talent_t corrupt_the_blood;
      player_talent_t lingering_darkness;
      player_talent_t symbolic_victory;

      player_talent_t ethereal_cloak;       // No implementation
      player_talent_t bait_and_switch;      // No implementation
      player_talent_t momentum_of_despair;
      player_talent_t follow_the_blood;
      player_talent_t shadewalker;
      player_talent_t shroud_of_night;      // No implementation

      player_talent_t darkest_night;

    } deathstalker;

    struct fatebound_talents_t
    {
      player_talent_t hand_of_fate;

      player_talent_t chosens_revelry;
      player_talent_t tempted_fate;     // TODO: Defensive
      player_talent_t mean_streak;
      player_talent_t inexorable_march; // No implementation
      player_talent_t deaths_arrival;   // NYI in-game

      player_talent_t deal_fate;
      player_talent_t edge_case;        // TODO: Double jeopardy + edge case stealth break overlap bug
      player_talent_t fate_intertwined;

      player_talent_t delivered_doom;
      player_talent_t inevitabile_end;
      player_talent_t destiny_defined;  // TODO: Outlaw also gets the poison proc rate the text says is for assa? Verify in-game.
      player_talent_t double_jeopardy;  // TODO: Double jeopardy + edge case stealth break overlap bug

      player_talent_t fateful_ending;   // TODO: Add tertiary stats

    } fatebound;

    struct trickster_talents_t
    {
      player_talent_t unseen_blade;

      player_talent_t surprising_strikes; 
      player_talent_t smoke;              // No implementation
      player_talent_t mirrors;            // No implementation
      player_talent_t flawless_form;

      player_talent_t so_tricky;          // No implementation
      player_talent_t dont_be_suspicious;
      player_talent_t devious_distraction;
      player_talent_t thousand_cuts;
      player_talent_t flickerstrike;      // TODO: Add time-based trigger opt

      player_talent_t disorienting_strikes;
      player_talent_t cloud_cover;
      player_talent_t no_scruples;
      player_talent_t nimble_flurry;

      player_talent_t coup_de_grace;

    } trickster;

  } talent;

  // Masteries
  struct masteries_t
  {
    // Assassination
    const spell_data_t* potent_assassin;
    // Outlaw
    const spell_data_t* main_gauche;
    const spell_data_t* main_gauche_attack;
    // Subtlety
    const spell_data_t* executioner;
  } mastery;

  // Legendary effects
  struct legendary_t
  {
  } legendary;

  // Procs
  struct procs_t
  {
    // Shared
    proc_t* supercharger_wasted;

    // Assassination
    proc_t* amplifying_poison_consumed;
    proc_t* amplifying_poison_deathmark_consumed;
    proc_t* serrated_bone_spike_refund;
    proc_t* serrated_bone_spike_waste;
    proc_t* serrated_bone_spike_waste_partial;

    // Outlaw
    proc_t* count_the_odds;
    proc_t* count_the_odds_capped;
    proc_t* count_the_odds_ambush;
    proc_t* count_the_odds_ss;
    proc_t* count_the_odds_dispatch;
    proc_t* count_the_odds_coup_de_grace;
    proc_t* roll_the_bones_1;
    proc_t* roll_the_bones_2;
    proc_t* roll_the_bones_3;
    proc_t* roll_the_bones_4;
    proc_t* roll_the_bones_5;
    proc_t* roll_the_bones_6;
    proc_t* roll_the_bones_wasted;

    // Subtlety
    proc_t* deepening_shadows;
    proc_t* flagellation_cp_spend;
    proc_t* weaponmaster;

    // Set Bonus
    proc_t* tww2_subtlety_4pc;

  } procs;

  // Set Bonus effects
  struct set_bonuses_t
  {
    const spell_data_t* tww1_assassination_2pc;
    const spell_data_t* tww1_assassination_4pc;
    const spell_data_t* tww1_outlaw_2pc;
    const spell_data_t* tww1_outlaw_4pc;
    const spell_data_t* tww1_subtlety_2pc;
    const spell_data_t* tww1_subtlety_4pc;

    const spell_data_t* tww2_assassination_2pc;
    const spell_data_t* tww2_assassination_4pc;
    const spell_data_t* tww2_outlaw_2pc;
    const spell_data_t* tww2_outlaw_4pc;
    const spell_data_t* tww2_subtlety_2pc;
    const spell_data_t* tww2_subtlety_4pc;
  } set_bonuses;

  // Options
  struct rogue_options_t
  {
    std::vector<size_t> fixed_rtb;
    std::vector<double> fixed_rtb_odds;
    int initial_combo_points = 0;
    int initial_shadow_techniques = -1;
    int initial_supercharged_cp = 0;
    bool rogue_ready_trigger = true;
    bool priority_rotation = false;
  } options;

  rogue_t( sim_t* sim, util::string_view name, race_e r = RACE_NIGHT_ELF ) :
    player_t( sim, ROGUE, name, r ),
    shadow_techniques_counter( 0 ),
    deathstalkers_mark_debuff( nullptr ),
    auto_attack( nullptr ), melee_main_hand( nullptr ), melee_off_hand( nullptr ),
    restealth_allowed( false ),
    buffs( buffs_t() ),
    cooldowns( cooldowns_t() ),
    gains( gains_t() ),
    spell( spells_t() ),
    spec( spec_t() ),
    talent( talents_t() ),
    mastery( masteries_t() ),
    legendary( legendary_t() ),
    procs( procs_t() ),
    set_bonuses( set_bonuses_t() ),
    options( rogue_options_t() )
  {
    // Cooldowns
    cooldowns.stealth                   = get_cooldown( "stealth" );

    cooldowns.adrenaline_rush           = get_cooldown( "adrenaline_rush" );
    cooldowns.between_the_eyes          = get_cooldown( "between_the_eyes" );
    cooldowns.blade_flurry              = get_cooldown( "blade_flurry" );
    cooldowns.blade_rush                = get_cooldown( "blade_rush" );
    cooldowns.blind                     = get_cooldown( "blind" );
    cooldowns.cloak_of_shadows          = get_cooldown( "cloak_of_shadows" );
    cooldowns.cold_blood                = get_cooldown( "cold_blood" );
    cooldowns.deathmark                 = get_cooldown( "deathmark" );
    cooldowns.evasion                   = get_cooldown( "evasion" );
    cooldowns.feint                     = get_cooldown( "feint" );
    cooldowns.flagellation              = get_cooldown( "flagellation" );
    cooldowns.garrote                   = get_cooldown( "garrote" );
    cooldowns.ghostly_strike            = get_cooldown( "ghostly_strike" );
    cooldowns.goremaws_bite             = get_cooldown( "goremaws_bite" );
    cooldowns.gouge                     = get_cooldown( "gouge" );
    cooldowns.grappling_hook            = get_cooldown( "grappling_hook" );
    cooldowns.keep_it_rolling           = get_cooldown( "keep_it_rolling" );
    cooldowns.killing_spree             = get_cooldown( "killing_spree" );
    cooldowns.kingsbane                 = get_cooldown( "kingsbane" );
    cooldowns.roll_the_bones            = get_cooldown( "roll_the_bones" );
    cooldowns.secret_technique          = get_cooldown( "secret_technique" );
    cooldowns.shadow_blades             = get_cooldown( "shadow_blades" );
    cooldowns.shadow_dance              = get_cooldown( "shadow_dance" );
    cooldowns.shadowstep                = get_cooldown( "shadowstep" );
    cooldowns.shiv                      = get_cooldown( "shiv" );
    cooldowns.shuriken_tornado          = get_cooldown( "shuriken_tornado" );
    cooldowns.sprint                    = get_cooldown( "sprint" );   
    cooldowns.symbols_of_death          = get_cooldown( "symbols_of_death" );
    cooldowns.thistle_tea               = get_cooldown( "thistle_tea" );
    cooldowns.unseen_blade_icd          = get_cooldown( "unseen_blade_icd" );
    cooldowns.vanish                    = get_cooldown( "vanish" );

    cooldowns.weaponmaster              = get_target_specific_cooldown( "weaponmaster" );

    resource_regeneration = regen_type::DYNAMIC;
    regen_caches[CACHE_HASTE] = true;
    regen_caches[CACHE_ATTACK_HASTE] = true;
  }

  // Character Definition
  void        init_spells() override;
  void        init_base_stats() override;
  void        init_talents() override;
  void        init_gains() override;
  void        init_procs() override;
  void        init_scaling() override;
  void        init_resources( bool force ) override;
  void        init_items() override;
  void        init_special_effects() override;
  void        init_finished() override;
  void        create_buffs() override;
  void        create_options() override;
  void        copy_from( player_t* source ) override;
  std::string create_profile( save_e stype ) override;
  void        init_action_list() override;
  void        reset() override;
  void        activate() override;
  void        arise() override;
  void        combat_begin() override;
  timespan_t  available() const override;
  action_t*   create_action( util::string_view name, util::string_view options ) override;
  std::unique_ptr<expr_t> create_action_expression( action_t& action, std::string_view name_str ) override;
  std::unique_ptr<expr_t> create_expression( util::string_view name_str ) override;
  std::unique_ptr<expr_t> create_resource_expression( util::string_view name ) override;
  void        regen( timespan_t periodicity ) override;
  resource_e  primary_resource() const override { return RESOURCE_ENERGY; }
  role_e      primary_role() const override  { return ROLE_ATTACK; }
  stat_e      convert_hybrid_stat( stat_e s ) const override;

  // Default consumables
  std::string default_potion() const override;
  std::string default_flask() const override;
  std::string default_food() const override;
  std::string default_rune() const override;
  std::string default_temporary_enchant() const override;

  double    composite_attribute_multiplier( attribute_e attr ) const override;
  double    composite_melee_auto_attack_speed() const override;
  double    composite_melee_haste() const override;
  double    composite_melee_crit_chance() const override;
  double    composite_spell_crit_chance() const override;
  double    composite_spell_haste() const override;
  double    composite_damage_versatility() const override;
  double    composite_heal_versatility() const override;
  double    composite_leech() const override;
  double    matching_gear_multiplier( attribute_e attr ) const override;
  double    composite_player_multiplier( school_e school ) const override;
  double    composite_player_pet_damage_multiplier( const action_state_t*, bool ) const override;
  double    composite_player_target_multiplier( player_t* target, school_e school ) const override;
  double    composite_player_target_crit_chance( player_t* target ) const override;
  double    composite_player_target_armor( player_t* target ) const override;
  double    resource_regen_per_second( resource_e ) const override;
  double    non_stacking_movement_modifier() const override;
  double    stacking_movement_modifier() const override;
  void      apply_affecting_auras( action_t& action ) override;
  void      invalidate_cache( cache_e ) override;

  void break_stealth();
  void cancel_auto_attacks() override;
  void do_exsanguinate( dot_t* dot, double coeff );

  void trigger_venomous_wounds_death( player_t* );

  double consume_cp_max() const
  {
    return COMBO_POINT_MAX + as<double>( talent.rogue.deeper_stratagem->effectN( 2 ).base_value() +
                                         talent.assassination.sanguine_stratagem->effectN( 2 ).base_value() +
                                         talent.outlaw.devious_stratagem->effectN( 2 ).base_value() +
                                         talent.subtlety.secret_stratagem->effectN( 2 ).base_value() );
  }

  double current_cp( bool /* react */ = false ) const
  {
    return resources.current[ RESOURCE_COMBO_POINT ];
  }

  // Current number of effective combo points, considering Supercharger and Escalating Blade
  double current_effective_cp( bool use_supercharger = true, bool react = false ) const
  {
    double current_cp = std::min( this->current_cp( react ), consume_cp_max() );

    if ( use_supercharger && current_cp > 0 )
    {
      if ( range::any_of( buffs.supercharger, []( const buff_t* buff ) { return buff->check(); } ) )
        current_cp += talent.rogue.supercharger->effectN( 2 ).base_value() + talent.rogue.forced_induction->effectN( 1 ).base_value();
    }

    return current_cp;
  }

  // Extra attack proc chance for Outlaw mechanics (Sinister Strike, Audacity, Hidden Opportunity)
  double extra_attack_proc_chance() const
  {
    double proc_chance = spec.sinister_strike->effectN( 3 ).percent();
    proc_chance += buffs.skull_and_crossbones->stack_value();
    if ( talent.fatebound.destiny_defined->ok() )
      proc_chance += talent.fatebound.destiny_defined->effectN( 2 ).percent();
    return proc_chance;
  }

  target_specific_t<rogue_td_t> target_data;

  const rogue_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  rogue_td_t* get_target_data( player_t* target ) const override
  {
    rogue_td_t*& td = target_data[ target ];
    if ( ! td )
    {
      td = new rogue_td_t( target, const_cast<rogue_t*>(this) );
    }
    return td;
  }

  static actions::rogue_attack_t* cast_attack( action_t* action )
  { return debug_cast<actions::rogue_attack_t*>( action ); }

  static const actions::rogue_attack_t* cast_attack( const action_t* action )
  { return debug_cast<const actions::rogue_attack_t*>( action ); }

  void swap_weapon( weapon_slot_e slot, current_weapon_e to_weapon, bool in_combat = true );
  bool stealthed( uint32_t stealth_mask = STEALTH_ALL ) const;

  // Secondary Action Tracking
private:
  std::vector<action_t*> background_actions;

public:
  template <typename T, typename... Ts>
  T* find_background_action( util::string_view n = {} )
  {
    T* found_action = nullptr;
    for ( auto action : background_actions )
    {
      found_action = dynamic_cast<T*>( action );
      if ( found_action )
      {
        if ( n.empty() || found_action->name_str == n )
          break;
        else
          found_action = nullptr;
      }
    }
    return found_action;
  }

  template <typename T, typename... Ts>
  T* get_background_action( util::string_view n, Ts&&... args )
  {
    auto it = range::find( background_actions, n, &action_t::name_str );
    if ( it != background_actions.cend() )
    {
      return dynamic_cast<T*>( *it );
    }

    auto action = new T( n, this, std::forward<Ts>( args )... );
    action->background = true;
    background_actions.push_back( action );
    return action;
  }

  // Secondary Action Trigger Tracking
private:
  std::vector<action_t*> secondary_trigger_actions;

public:
  template <typename T, typename... Ts>
  T* find_secondary_trigger_action( secondary_trigger source, util::string_view n = {} )
  {
    T* found_action = nullptr;
    for ( auto action : secondary_trigger_actions )
    {
      found_action = dynamic_cast<T*>( action );
      if ( found_action )
      {
        if ( found_action->secondary_trigger_type == source && ( n.empty() || found_action->name_str == n ) )
          break;
        else
          found_action = nullptr;
      }
    }
    return found_action;
  }

  template <typename T, typename... Ts>
  T* get_secondary_trigger_action( secondary_trigger source, util::string_view n, Ts&&... args )
  {
    T* found_action = find_secondary_trigger_action<T>( source, n );
    if ( !found_action )
    {
      // Actions will follow form of foo_t( util::string_view name, rogue_t* p, ... )
      found_action = new T( n, this, std::forward<Ts>( args )... );
      found_action->background = found_action->dual = true;
      found_action->repeating = false;
      found_action->secondary_trigger_type = source;
      secondary_trigger_actions.push_back( found_action );
    }
    return found_action;
  }
};

namespace actions { // namespace actions

// ==========================================================================
// Secondary Action Triggers
// ==========================================================================

template<typename Base>
struct secondary_action_trigger_t : public event_t
{
  Base* action;
  action_state_t* state;
  player_t* target;
  int cp;

  secondary_action_trigger_t( action_state_t* s, timespan_t delay = timespan_t::zero() ) :
    event_t( *s -> action -> sim, delay ), action( dynamic_cast<Base*>( s->action ) ), state( s ), target( nullptr ), cp( 0 )
  {}

  secondary_action_trigger_t( player_t* target, Base* action, int cp, timespan_t delay = timespan_t::zero() ) :
    event_t( *action -> sim, delay ), action( action ), state( nullptr ), target( target ), cp( cp )
  {}

  const char* name() const override
  { return "secondary_action_trigger"; }

  void execute() override
  {
    assert( action->is_secondary_action() );

    player_t* action_target = state ? state->target : target;

    // Ensure target is still available and did not demise during delay.
    if ( !action_target || action_target->is_sleeping() )
      return;

    action->set_target( action_target );

    // No state, construct one and grab combo points from the event instead of current CP amount.
    if ( !state )
    {
      state = action->get_state();
      state->target = action_target;
      action->cast_state( state )->set_combo_points( cp, cp );
      // Calling snapshot_internal, snapshot_state would overwrite CP.
      action->snapshot_internal( state, action->snapshot_flags, action->amount_type( state ) );
    }
    
    assert( !action->pre_execute_state );

    action->pre_execute_state = state;
    action->execute();
    state = nullptr;
  }

  ~secondary_action_trigger_t() override
  { if ( state ) action_state_t::release( state ); }
};

// ==========================================================================
// Rogue Action State
// ==========================================================================

template<typename T_ACTION>
struct rogue_action_state_t : public action_state_t
{
private:
  T_ACTION* action;
  int base_cp;
  int total_cp;
  double exsanguinated_rate;
  bool exsanguinated;

public:
  rogue_action_state_t( action_t* action, player_t* target ) :
    action_state_t( action, target ),
    action( dynamic_cast<T_ACTION*>( action ) ),
    base_cp( 0 ), total_cp( 0 ), exsanguinated_rate( 1.0 ), exsanguinated( false )
  {}

  void initialize() override
  {
    action_state_t::initialize();
    base_cp = 0;
    total_cp = 0;
    exsanguinated_rate = 1.0;
    exsanguinated = false;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    action_state_t::debug_str( s ) << " base_cp=" << base_cp << " total_cp=" << total_cp << " exsanguinated_rate=" << exsanguinated_rate;
    return s;
  }

  void copy_state( const action_state_t* s, bool copy_exsang )
  {
    action_state_t::copy_state( s );
    const rogue_action_state_t* rs = debug_cast<const rogue_action_state_t*>( s );
    base_cp = rs->base_cp;
    total_cp = rs->total_cp;
    if ( copy_exsang )
    {
      exsanguinated_rate = rs->exsanguinated_rate;
      exsanguinated = rs->exsanguinated;
    }
    else
    {
      exsanguinated_rate = 1.0;
      exsanguinated = false;
    }
  }

  void copy_state( const action_state_t* s ) override
  {
    // Default copy behavior to not copy Exsanguianted DoT state
    this->copy_state( s, false );
  }

  T_ACTION* get_action() const
  {
    return action;
  }

  void set_combo_points( int base_cp, int total_cp )
  {
    this->base_cp = base_cp;
    this->total_cp = total_cp;
  }

  int get_combo_points( bool base_only = false ) const
  {
    if ( base_only )
      return base_cp;

    return total_cp;
  }

  void set_exsanguinated_rate( double rate )
  {
    exsanguinated_rate = rate;
    exsanguinated = true;
  }

  double get_exsanguinated_rate() const
  {
    return exsanguinated_rate;
  }

  double is_exsanguinated() const
  {
    return exsanguinated;
  }

  void clear_exsanguinated()
  {
    exsanguinated_rate = 1.0;
    exsanguinated = false;
  }

  proc_types2 cast_proc_type2() const override
  {
    if( action->secondary_trigger_type == secondary_trigger::WEAPONMASTER )
    {
      return PROC2_CAST_DAMAGE;
    }

    return action_state_t::cast_proc_type2();
  }
};

// ==========================================================================
// Rogue Action Template
// ==========================================================================

template <typename Base>
class rogue_action_t : public Base
{
protected:
  /// typedef for rogue_action_t<action_base_t>
  using base_t = rogue_action_t<Base>;

private:
  /// typedef for the templated action type, eg. spell_t, attack_t, heal_t
  using ab = Base;
  bool _requires_stealth;
  bool _breaks_stealth;

public:
  // Secondary triggered ability, due to Weaponmaster talent or Death from Above. Secondary
  // triggered abilities cost no resources or incur cooldowns.
  secondary_trigger secondary_trigger_type;

  proc_t* supercharged_cp_proc;
  proc_t* cold_blood_consumed_proc;
  proc_t* perforated_veins_consumed_proc;

  // Affect flags for various dynamic effects
  struct damage_affect_data
  {
    bool direct = false;
    double direct_percent = 0.0;
    bool periodic = false;
    double periodic_percent = 0.0;
  };
  struct
  {
    bool adrenaline_rush_gcd = false;
    bool alacrity = false;
    bool audacity = false;              // Stance Mask
    bool blindside = false;             // Stance Mask
    bool broadside_cp = false;
    bool cold_blood = false;
    bool danse_macabre = false;         // Trigger
    bool darkest_night = false;         // Damage
    bool darkest_night_crit = false;    // Crit%
    bool dashing_scoundrel = false;
    bool deathmark = false;             // Tuning Aura
    bool destiny_defined = false;       // Proc Increase
    bool deepening_shadows = false;     // Trigger
    bool dragon_tempered_blades = false;// Proc Reduction
    bool fazed_damage = false;
    bool fazed_crit_chance = false;
    bool fazed_crit_damage = false;
    bool flagellation = false;
    bool ghostly_strike = false;
    bool goremaws_bite = false;         // Cost Reduction
    bool improved_ambush = false;
    bool improved_shiv = false;
    bool lethal_dose = false;
    bool maim_mangle = false;           // Renamed Systemic Failure for DF talent
    bool master_assassin = false;
    bool momentum_of_despair = false;   // Crit Damage Multiplier
    bool relentless_strikes = false;    // Trigger
    bool ruthlessness = false;          // Trigger
    bool shadow_blades_cp = false;
    bool zoldyck_insignia = false;

    damage_affect_data deeper_daggers;
    damage_affect_data follow_the_blood;
    damage_affect_data mastery_executioner;
    damage_affect_data mastery_potent_assassin;
    damage_affect_data tww2_subtlety_4pc;
  } affected_by;

  std::vector<damage_buff_t*> direct_damage_buffs;
  std::vector<damage_buff_t*> periodic_damage_buffs;
  std::vector<damage_buff_t*> auto_attack_damage_buffs;
  std::vector<damage_buff_t*> crit_chance_buffs;
  
  struct consume_buff_t
  {
    buff_t* buff = nullptr;
    proc_t* proc = nullptr;
    timespan_t delay = timespan_t::zero();
    bool on_background = false;
    bool decrement = false;
  };
  std::vector<consume_buff_t> consume_buffs;

  // Init =====================================================================

  rogue_action_t( util::string_view n, rogue_t* p, const spell_data_t* s = spell_data_t::nil(),
                  util::string_view options = {} )
    : ab( n, p, s ),
      _requires_stealth( false ),
      _breaks_stealth( true ),
      secondary_trigger_type( secondary_trigger::NONE ),
      supercharged_cp_proc( nullptr ),
      cold_blood_consumed_proc( nullptr ),
      perforated_veins_consumed_proc( nullptr )
  {
    ab::parse_options( options );
    parse_spell_data( s );

    // rogue_t sets base and min GCD to 1s by default but let's also enforce non-hasted GCDs.
    // Even for rogue abilities that can be considered spells, hasted GCDs seem to be an exception rather than rule.
    // Those should be set explicitly. (see Vendetta, Shadow Blades, Detection)
    ab::gcd_type = gcd_haste_type::NONE;

    // Affecting Passive Auras
    // Put ability specific ones here; class/spec wide ones with labels that can effect things like trinkets in rogue_t::apply_affecting_auras.

    // Affecting Passive Spells
    ab::apply_affecting_aura( p->spec.shadowstep );

    // Affecting Passive Talents
    ab::apply_affecting_aura( p->talent.rogue.master_poisoner );
    ab::apply_affecting_aura( p->talent.rogue.nimble_fingers );
    ab::apply_affecting_aura( p->talent.rogue.rushed_setup );
    ab::apply_affecting_aura( p->talent.rogue.improved_sprint );
    ab::apply_affecting_aura( p->talent.rogue.thrill_seeking );
    ab::apply_affecting_aura( p->talent.rogue.deadly_precision );
    ab::apply_affecting_aura( p->talent.rogue.virulent_poisons );
    ab::apply_affecting_aura( p->talent.rogue.tight_spender );
    ab::apply_affecting_aura( p->talent.rogue.lethality );
    ab::apply_affecting_aura( p->talent.rogue.deeper_stratagem );
    ab::apply_affecting_aura( p->talent.rogue.subterfuge );
    ab::apply_affecting_aura( p->talent.rogue.without_a_trace );

    ab::apply_affecting_aura( p->talent.assassination.bloody_mess );
    ab::apply_affecting_aura( p->talent.assassination.thrown_precision );
    ab::apply_affecting_aura( p->talent.assassination.lightweight_shiv );
    ab::apply_affecting_aura( p->talent.assassination.flying_daggers );
    ab::apply_affecting_aura( p->talent.assassination.sanguine_stratagem );
    ab::apply_affecting_aura( p->talent.assassination.vicious_venoms );
    ab::apply_affecting_aura( p->talent.assassination.fatal_concoction );
    ab::apply_affecting_aura( p->talent.assassination.tiny_toxic_blade );
    ab::apply_affecting_aura( p->talent.assassination.shrouded_suffocation );
    ab::apply_affecting_aura( p->talent.assassination.arterial_precision );
    ab::apply_affecting_aura( p->talent.assassination.sudden_demise );

    ab::apply_affecting_aura( p->talent.outlaw.retractable_hook );
    ab::apply_affecting_aura( p->talent.outlaw.blinding_powder );
    ab::apply_affecting_aura( p->talent.outlaw.improved_between_the_eyes );
    ab::apply_affecting_aura( p->talent.outlaw.dirty_tricks );
    ab::apply_affecting_aura( p->talent.outlaw.heavy_hitter );
    ab::apply_affecting_aura( p->talent.outlaw.devious_stratagem );
    ab::apply_affecting_aura( p->talent.outlaw.underhanded_upper_hand );
    ab::apply_affecting_aura( p->talent.outlaw.precision_shot );

    // 2025-04-01 -- Bonus damage is not applied to Blade Flurry
    if ( !p->bugs )
    {
      ab::apply_affecting_aura( p->talent.outlaw.deft_maneuvers );
    }

    ab::apply_affecting_aura( p->talent.subtlety.improved_backstab );
    ab::apply_affecting_aura( p->talent.subtlety.improved_shuriken_storm );
    ab::apply_affecting_aura( p->talent.subtlety.quick_decisions );
    ab::apply_affecting_aura( p->talent.subtlety.veiltouched );
    ab::apply_affecting_aura( p->talent.subtlety.swift_death );
    ab::apply_affecting_aura( p->talent.subtlety.improved_shadow_dance );
    ab::apply_affecting_aura( p->talent.subtlety.double_dance );
    ab::apply_affecting_aura( p->talent.subtlety.secret_stratagem );
    ab::apply_affecting_aura( p->talent.subtlety.death_perception );
    ab::apply_affecting_aura( p->talent.subtlety.dark_brew );

    ab::apply_affecting_aura( p->talent.trickster.disorienting_strikes );
    ab::apply_affecting_aura( p->talent.trickster.dont_be_suspicious );

    // Dynamically affected flags
    // Special things like CP, Energy, Crit, etc.
    affected_by.improved_ambush = ab::data().affected_by( p->talent.rogue.improved_ambush->effectN( 1 ) );

    if ( p->talent.deathstalker.momentum_of_despair->ok() )
    {
      affected_by.momentum_of_despair = ab::data().affected_by( p->spell.momentum_of_despair_buff->effectN( 2 ) );
    }

    if ( p->talent.trickster.unseen_blade->ok() )
    {
      affected_by.fazed_damage = ab::data().affected_by( p->spell.fazed_debuff->effectN( 1 ) );
      affected_by.fazed_crit_damage = ab::data().affected_by( p->spell.fazed_debuff->effectN( 4 ) );
      affected_by.fazed_crit_chance = ab::data().affected_by( p->spell.fazed_debuff->effectN( 5 ) );
    }

    if ( p->talent.fatebound.destiny_defined->ok() )
    {
      affected_by.destiny_defined = ab::data().affected_by( p->talent.fatebound.destiny_defined->effectN( 1 ) );
    }
    
    // Assassination
    affected_by.blindside = ab::data().affected_by( p->spec.blindside_buff->effectN( 1 ) );
    affected_by.master_assassin = ab::data().affected_by( p->spec.master_assassin_buff->effectN( 1 ) );

    affected_by.improved_shiv =
      ( p->talent.assassination.improved_shiv->ok() && ab::data().affected_by( p->spec.improved_shiv_debuff->effectN( 1 ) ) ) ||
      ( ( p->talent.assassination.arterial_precision->ok() && ab::data().affected_by( p->spec.improved_shiv_debuff->effectN( 3 ) ) ) ||
        ab::data().affected_by_label( p->spec.improved_shiv_debuff->effectN( 4 ) ) );

    if ( p->talent.assassination.systemic_failure->ok() )
    {
      affected_by.maim_mangle = ab::data().affected_by( p->spec.garrote->effectN( 4 ) );
    }

    if ( p->talent.assassination.dashing_scoundrel->ok() )
    {
      affected_by.dashing_scoundrel = ab::data().affected_by( p->spec.envenom->effectN( 5 ) );
    }

    if ( p->talent.assassination.zoldyck_recipe->ok() )
    {
      affected_by.zoldyck_insignia = ab::data().affected_by( p->mastery.potent_assassin->effectN( 1 ) ) ||
                                     ab::data().affected_by( p->mastery.potent_assassin->effectN( 2 ) ) ||
                                     ab::data().affected_by_label( p->mastery.potent_assassin->effectN( 3 ) );
    }

    if ( p->talent.assassination.lethal_dose->ok() )
    {
      affected_by.lethal_dose = ab::data().affected_by( p->mastery.potent_assassin->effectN( 1 ) ) ||
                                ab::data().affected_by( p->mastery.potent_assassin->effectN( 2 ) );
    }

    if ( p->talent.assassination.deathmark->ok() )
    {
      affected_by.deathmark = ab::data().affected_by( p->spec.deathmark_debuff->effectN( 1 ) ) ||
                              ab::data().affected_by( p->spec.deathmark_debuff->effectN( 2 ) );
    }

    if ( p->talent.assassination.dragon_tempered_blades->ok() )
    {
      affected_by.dragon_tempered_blades = ab::data().affected_by( p->talent.assassination.dragon_tempered_blades->effectN( 2 ) );
    }

    // Outlaw
    affected_by.adrenaline_rush_gcd = ab::data().affected_by( p->talent.outlaw.adrenaline_rush->effectN( 3 ) );

    affected_by.broadside_cp = ( ab::data().affected_by( p->spec.broadside->effectN( 1 ) ) ||
                                 ab::data().affected_by( p->spec.broadside->effectN( 2 ) ) ||
                                 ab::data().affected_by( p->spec.broadside->effectN( 3 ) ) );

    affected_by.audacity = ab::data().affected_by( p->spec.audacity_buff->effectN( 1 ) );

    if ( p->talent.outlaw.ghostly_strike->ok() )
    {
      affected_by.ghostly_strike = ab::data().affected_by( p->talent.outlaw.ghostly_strike->effectN( 3 ) );
    }

    // Subtlety
    affected_by.shadow_blades_cp = ( ab::data().affected_by( p->talent.subtlety.shadow_blades->effectN( 2 ) ) ||
                                     ab::data().affected_by( p->talent.subtlety.shadow_blades->effectN( 3 ) ) ||
                                     ab::data().affected_by( p->talent.subtlety.shadow_blades->effectN( 4 ) ) );

    affected_by.danse_macabre = ab::data().affected_by( p->spec.danse_macabre_buff->effectN( 1 ) );

    if ( p->talent.subtlety.goremaws_bite->ok() && p->spec.goremaws_bite_buff->ok() )
    {
      affected_by.goremaws_bite = ab::data().affected_by( p->spec.goremaws_bite_buff->effectN( 2 ) );
    }

    // Auto-parsing for damage affecting dynamic flags
    auto parse_damage_affecting_spell = [this]( const spell_data_t* spell, damage_affect_data& flags )
    {
      if ( !spell->ok() )
        return;

      for ( const spelleffect_data_t& effect : spell->effects() )
      {
        if ( !effect.ok() || !( effect.type() == E_APPLY_AURA ||
                                effect.subtype() == A_ADD_PCT_MODIFIER ||
                                effect.subtype() == A_ADD_PCT_LABEL_MODIFIER ) )
          continue;

        if ( ab::data().affected_by_all( effect ) )
        {
          switch ( effect.misc_value1() )
          {
            case P_GENERIC:
              flags.direct = true;
              flags.direct_percent = effect.percent();
              break;
            case P_TICK_DAMAGE:
              flags.periodic = true;
              flags.periodic_percent = effect.percent();
              break;
          }
        }
      }
    };

    parse_damage_affecting_spell( p->spec.deeper_daggers_buff, affected_by.deeper_daggers );
    parse_damage_affecting_spell( p->mastery.executioner, affected_by.mastery_executioner );
    parse_damage_affecting_spell( p->mastery.potent_assassin, affected_by.mastery_potent_assassin );

    // Check for dark_brew synergy with Veiltouched
    if ( p->talent.subtlety.dark_brew->ok() && ab::data().affected_by( p->talent.subtlety.dark_brew->effectN( 1 ) ) )
    {
      if ( p->talent.subtlety.veiltouched->ok() )
      {
        damage_affect_data passive_list;
        parse_damage_affecting_spell( p->talent.subtlety.veiltouched, passive_list );
        if ( !passive_list.direct )
        {
          const spelleffect_data_t& effect = p->talent.subtlety.veiltouched->effectN( 1 );
          ab::base_dd_multiplier *= ( 1 + effect.percent() );
          p->sim->print_debug( "{} {} is manually affected by Veiltouched (id={} - effect #{})", *p, *this,
                               effect.id(), effect.spell_effect_num() + 1 );
          p->sim->print_debug( "{} base_dd_multiplier modified by {}% to {}", *this, effect.base_value(), ab::base_dd_multiplier );
          ab::affecting_list.emplace_back( &effect, effect.percent() );
        }
        if ( !passive_list.periodic )
        {
          const spelleffect_data_t& effect = p->talent.subtlety.veiltouched->effectN( 2 );
          ab::base_td_multiplier *= ( 1 + effect.percent() );
          p->sim->print_debug( "{} {} is manually affected by Veiltouched (id={} - effect #{})", *p, *this,
                               effect.id(), effect.spell_effect_num() + 1 );
          p->sim->print_debug( "{} base_td_multiplier modified by {}% to {}", *this, effect.base_value(), ab::base_td_multiplier );
          ab::affecting_list.emplace_back( &effect, effect.percent() );
        }
      }

      if ( p->talent.subtlety.deeper_daggers->ok() )
      {
        if ( !affected_by.deeper_daggers.direct )
          affected_by.deeper_daggers.direct = true;
        if ( !affected_by.deeper_daggers.periodic )
          affected_by.deeper_daggers.periodic = true;
      }
    }

    if ( p->set_bonuses.tww2_subtlety_4pc->ok() )
    {
      auto buff_spell = p->set_bonuses.tww2_subtlety_2pc->effectN( 1 ).trigger();

      affected_by.tww2_subtlety_4pc.direct_percent = p->set_bonuses.tww2_subtlety_4pc->effectN( 1 ).percent();
      affected_by.tww2_subtlety_4pc.direct = ab::data().affected_by( buff_spell->effectN( 2 ) ) ||
        ab::data().affected_by_label( buff_spell->effectN( 4 ) ) ||
        ab::data().affected_by_label( buff_spell->effectN( 6 ) );

      affected_by.tww2_subtlety_4pc.periodic_percent = p->set_bonuses.tww2_subtlety_4pc->effectN( 1 ).percent();
      affected_by.tww2_subtlety_4pc.periodic = ab::data().affected_by( buff_spell->effectN( 3 ) ) ||
        ab::data().affected_by_label( buff_spell->effectN( 5 ) ) ||
        ab::data().affected_by_label( buff_spell->effectN( 7 ) );
    }
  }

  void init() override
  {
    ab::init();
    
    if ( consumes_supercharger() )
    {
      supercharged_cp_proc = p()->get_proc( "Supercharger " + ab::name_str );
    }

    if ( p()->buffs.cold_blood->is_affecting( &ab::data() ) )
    {
      cold_blood_consumed_proc = p()->get_proc( "Cold Blood " + ab::name_str );
    }

    if ( p()->buffs.perforated_veins->is_affecting( &ab::data() ) )
    {
      perforated_veins_consumed_proc = p()->get_proc( "Perforated Veins " + ab::name_str );
    }

    auto register_damage_buff = [ this ]( damage_buff_t* buff ) {
      if ( buff->is_affecting_direct( ab::s_data ) )
        direct_damage_buffs.push_back( buff );

      if ( buff->is_affecting_periodic( ab::s_data ) )
        periodic_damage_buffs.push_back( buff );

      if ( ab::repeating && !ab::special && !ab::s_data->ok() && buff->auto_attack_mod.multiplier != 1.0 )
        auto_attack_damage_buffs.push_back( buff );

      if ( buff->is_affecting_crit_chance( ab::s_data ) )
        crit_chance_buffs.push_back( buff );
    };

    direct_damage_buffs.clear();
    periodic_damage_buffs.clear();
    auto_attack_damage_buffs.clear();
    crit_chance_buffs.clear();

    register_damage_buff( p()->buffs.acrobatic_strikes );
    register_damage_buff( p()->buffs.broadside );
    register_damage_buff( p()->buffs.grand_melee );
    register_damage_buff( p()->buffs.cold_blood );
    register_damage_buff( p()->buffs.danse_macabre );
    register_damage_buff( p()->buffs.deathstalkers_mark );
    register_damage_buff( p()->buffs.fatebound_coin_heads );
    register_damage_buff( p()->buffs.finality_eviscerate );
    register_damage_buff( p()->buffs.finality_black_powder );
    register_damage_buff( p()->buffs.flawless_form );
    register_damage_buff( p()->buffs.kingsbane );
    register_damage_buff( p()->buffs.master_assassin );
    register_damage_buff( p()->buffs.master_assassin_aura );
    register_damage_buff( p()->buffs.momentum_of_despair );
    register_damage_buff( p()->buffs.perforated_veins );
    register_damage_buff( p()->buffs.ruthless_precision );
    register_damage_buff( p()->buffs.shadow_dance );
    register_damage_buff( p()->buffs.summarily_dispatched );
    register_damage_buff( p()->buffs.symbolic_victory );
    register_damage_buff( p()->buffs.symbols_of_death );
    register_damage_buff( p()->buffs.the_rotten );

    register_damage_buff( p()->buffs.tww1_assassination_2pc );
    register_damage_buff( p()->buffs.tww1_assassination_4pc );
    register_damage_buff( p()->buffs.tww1_outlaw_4pc );
    register_damage_buff( p()->buffs.tww1_subtlety_2pc );
    register_damage_buff( p()->buffs.tww1_subtlety_4pc );

    register_damage_buff( p()->buffs.tww2_assassination_2pc );
    register_damage_buff( p()->buffs.tww2_assassination_4pc );
    register_damage_buff( p()->buffs.tww2_outlaw_2pc );
    register_damage_buff( p()->buffs.tww2_subtlety_2pc );   

    if ( ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 )
    {
      affected_by.alacrity = true;
      affected_by.deepening_shadows = true;
      affected_by.flagellation = true;
      affected_by.relentless_strikes = true;
      affected_by.ruthlessness = true;
    }

    // Auto-Consume Buffs on Execute
    auto register_consume_buff = [this]( buff_t* buff, bool condition, proc_t* proc = nullptr, timespan_t delay = timespan_t::zero(),
                                         bool on_background = false, bool decrement = false )
    {
      if ( condition )
      {
        consume_buffs.push_back( { buff, proc, delay, on_background, decrement } );
      }
    };

    register_consume_buff( p()->buffs.audacity, affected_by.audacity );
    register_consume_buff( p()->buffs.blindside, affected_by.blindside );
    // Special case for Coup de Grace as it is not consumed until the final impact
    // Killing Spree does not consume until the last_tick
    register_consume_buff( p()->buffs.cold_blood, ( p()->buffs.cold_blood->is_affecting( &ab::data() ) &&
                                                    ab::data().id() != p()->talent.subtlety.secret_technique->id() &&
                                                    ab::data().id() != p()->talent.outlaw.killing_spree->id() &&
                                                    ab::data().id() != p()->spell.coup_de_grace->id() &&
                                                    ( secondary_trigger_type != secondary_trigger::COUP_DE_GRACE ||
                                                      ab::data().id() == p()->spell.coup_de_grace_damage_3->id() ) ),
                           cold_blood_consumed_proc, 0_s, false, p()->talent.fatebound.inevitabile_end->ok() );
    register_consume_buff( p()->buffs.deathstalkers_mark, p()->buffs.deathstalkers_mark->is_affecting( &ab::data() ),
                           nullptr, 1_ms );
    register_consume_buff( p()->buffs.goremaws_bite, affected_by.goremaws_bite );
    register_consume_buff( p()->buffs.perforated_veins, p()->buffs.perforated_veins->is_affecting( &ab::data() ),
                           perforated_veins_consumed_proc, 1_ms );
    register_consume_buff( p()->buffs.symbolic_victory, p()->buffs.symbolic_victory->is_affecting( &ab::data() ),
                           nullptr, p()->bugs ? 0_ms : 1_ms );
    register_consume_buff( p()->buffs.the_rotten, p()->buffs.the_rotten->is_affecting_crit_chance( &ab::data() ), nullptr, 1_ms, false, true );
    
    register_consume_buff( p()->buffs.tww1_subtlety_2pc, p()->buffs.tww1_subtlety_2pc->is_affecting( &ab::data() ),
                           nullptr, 1.31_s );
    register_consume_buff( p()->buffs.tww1_outlaw_4pc, p()->buffs.tww1_outlaw_4pc->is_affecting( &ab::data() ) );
  }

  // Type Wrappers ============================================================

  static const rogue_action_state_t<base_t>* cast_state( const action_state_t* st )
  { return debug_cast<const rogue_action_state_t<base_t>*>( st ); }

  static rogue_action_state_t<base_t>* cast_state( action_state_t* st )
  { return debug_cast<rogue_action_state_t<base_t>*>( st ); }

  rogue_t* p()
  { return debug_cast<rogue_t*>( ab::player ); }

  const rogue_t* p() const
  { return debug_cast<const rogue_t*>( ab::player ); }

  rogue_td_t* td( player_t* t ) const
  { return p()->get_target_data( t ); }

  // Spell Data Helpers =======================================================

  void parse_spell_data( const spell_data_t* s )
  {
    if ( s->stance_mask() & 0x20000000)
      _requires_stealth = true;

    if ( s->flags( spell_attribute::SX_NO_STEALTH_BREAK ) )
      _breaks_stealth = false;

    for ( size_t i = 1; i <= s->effect_count(); i++ )
    {
      const spelleffect_data_t& effect = s->effectN( i );

      switch ( effect.type() )
      {
        case E_ADD_COMBO_POINTS:
          if ( ab::energize_type != action_energize::NONE )
          {
            ab::energize_type = action_energize::ON_HIT;
            ab::energize_amount = effect.base_value();
            ab::energize_resource = RESOURCE_COMBO_POINT;
          }
          break;
        default:
          break;
      }

      if ( effect.type() == E_APPLY_AURA && effect.subtype() == A_PERIODIC_DAMAGE )
      {
        ab::base_ta_adder = effect.bonus( p() );
      }
      else if ( effect.type() == E_SCHOOL_DAMAGE )
      {
        ab::base_dd_adder = effect.bonus( p() );
      }
    }
  }

  // Action State =============================================================

  action_state_t* new_state() override
  {
    return new rogue_action_state_t<base_t>( this, ab::target );
  }

  void update_state( action_state_t* state, unsigned flags, result_amount_type rt ) override
  {
    if ( cast_state( state )->is_exsanguinated() )
    {
      flags &= ~STATE_HASTE;
    }
    
    ab::update_state( state, flags, rt );
  }

  void snapshot_state( action_state_t* state, result_amount_type rt ) override
  {
    int consume_cp = as<int>( std::min( p()->current_cp(), p()->consume_cp_max() ) );
    int effective_cp = consume_cp;

    // Apply and Snapshot Supercharger Buffs
    if ( p()->talent.rogue.supercharger->ok() && consumes_supercharger() )
    {
      if ( range::any_of( p()->buffs.supercharger, []( const buff_t* buff ) { return buff->check(); } ) )
        effective_cp += p()->talent.rogue.supercharger->effectN( 2 ).base_value() +
                        p()->talent.rogue.forced_induction->effectN( 1 ).base_value();
    }

    auto rs = cast_state( state );
    rs->set_combo_points( consume_cp, effective_cp );
    rs->clear_exsanguinated();

    ab::snapshot_state( state, rt );
  }

  // Secondary Trigger Functions ==============================================

  bool is_secondary_action() const
  { return secondary_trigger_type != secondary_trigger::NONE && ab::background == true; }

  virtual void trigger_secondary_action( player_t* target, int cp = 0, timespan_t delay = timespan_t::zero() )
  {
    assert( is_secondary_action() );
    make_event<secondary_action_trigger_t<base_t>>( *ab::sim, target, this, cp, delay );
  }

  virtual void trigger_secondary_action( player_t* target, timespan_t delay )
  {
    trigger_secondary_action( target, 0, delay );
  }

  virtual void trigger_secondary_action( action_state_t* s, timespan_t delay = timespan_t::zero() )
  {
    assert( is_secondary_action() && s->action == this );
    make_event<secondary_action_trigger_t<base_t>>( *ab::sim, s, delay );
  }

  // Residual Trigger Functions ===============================================

  virtual void trigger_residual_action( const action_state_t* s, double multiplier = 1.0, bool unmitigated = true, bool reverse_target_da_multiplier = true, player_t* override_target = nullptr, bool trigger_event = true )
  {
    const double base_damage = unmitigated ? s->result_total : s->result_amount;
    const double target_da_multiplier = ( unmitigated && reverse_target_da_multiplier ) ? ( 1.0 / s->target_da_multiplier ) : 1.0;
    const double amount = base_damage * multiplier * target_da_multiplier;

    if ( amount <= 0 )
      return;

    player_t* primary_target = override_target ? override_target : s->target;

    p()->sim->print_debug( "{} triggers residual {} for {:.2f} damage ({:.2f} * {} * {:.3f}) on {}",
                           *p(), *this, amount, base_damage, multiplier, target_da_multiplier, *primary_target );

    if ( !ab::callbacks || !trigger_event )
    {
      ab::execute_on_target( primary_target, amount );
    }
    else
    {
      make_event( *p()->sim, 0_ms, [ this, amount, primary_target ]() {
        ab::execute_on_target( primary_target, amount );
      } );
    }
  }

  virtual void trigger_residual_action( player_t* primary_target, double amount, bool trigger_event = true )
  {
    if ( amount <= 0 )
      return;

    p()->sim->print_debug( "{} triggers residual {} for {:.2f} damage on {}", *p(), *this, amount, *primary_target );

    if ( !ab::callbacks || !trigger_event )
    {
      ab::execute_on_target( primary_target, amount );
    }
    else
    {
      make_event( *p()->sim, 0_ms, [ this, amount, primary_target ]() {
        ab::execute_on_target( primary_target, amount );
      } );
    }
  }

  // Helper Functions =========================================================

  virtual double generate_cp() const
  {
    double cp = 0;

    if ( ab::energize_type != action_energize::NONE && ab::energize_resource == RESOURCE_COMBO_POINT )
    {
      cp += ab::energize_amount;

      if ( affected_by.broadside_cp )
      {
        cp += p()->buffs.broadside->check_value();
      }

      if ( affected_by.shadow_blades_cp && p()->buffs.shadow_blades->check() )
      {
        cp += p()->buffs.shadow_blades->data().effectN( 2 ).base_value();
      }

      if ( affected_by.improved_ambush )
      {
        cp += p()->talent.rogue.improved_ambush->effectN( 1 ).base_value();
      }
    }

    return cp;
  }

  virtual bool procs_poison() const
  { return ab::weapon != nullptr && ab::has_amount_result(); }

  virtual bool procs_deadly_poison() const
  { return procs_poison(); }

  virtual bool procs_main_gauche() const
  { return false; }

  virtual bool procs_fatal_flourish() const
  { return ab::callbacks && !ab::proc && ab::weapon != nullptr && ab::weapon->slot == SLOT_OFF_HAND; }

  virtual bool procs_blade_flurry() const
  { return false; }

  virtual bool procs_nimble_flurry() const
  { return false; }

  virtual bool procs_shadow_blades_damage() const
  { return true; }

  virtual bool procs_caustic_spatter() const
  { return dbc::has_common_school( ab::get_school(), SCHOOL_NATURE ); }

  virtual bool procs_seal_fate() const
  { return ab::energize_type != action_energize::NONE && ab::energize_resource == RESOURCE_COMBO_POINT && ab::energize_amount > 0; }
  
  virtual bool procs_deal_fate() const
  { return false; }

  virtual bool requires_stealth() const
  {
    if ( affected_by.audacity && p()->buffs.audacity->check() )
      return false;

    if ( affected_by.blindside && p()->buffs.blindside->check() )
      return false;

    return _requires_stealth;
  }

  virtual bool breaks_stealth() const
  { return _breaks_stealth; }

  virtual bool consumes_supercharger() const
  { return ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 && ( ab::attack_power_mod.direct > 0.0 || ab::attack_power_mod.tick > 0.0 ); }

  double parry_chance( double exp, player_t* target ) const override
  {
    auto chance = ab::parry_chance(exp, target);
    if ( chance > 0.0 && td( target )->debuffs.fazed->up() )
    {
      chance += td( target )->debuffs.fazed->data().effectN( 2 ).percent();
    }
    return std::max(0.0, chance);
  }

public:
  // Ability triggers
  void spend_combo_points( const action_state_t* );
  void trigger_auto_attack( const action_state_t* );
  void trigger_poisons( const action_state_t* );
  void trigger_seal_fate( const action_state_t* );
  void trigger_main_gauche( const action_state_t* );
  void trigger_fatal_flourish( const action_state_t* );
  void trigger_energy_refund();
  void trigger_doomblade( const action_state_t* );
  void trigger_poison_bomb( const action_state_t* );
  void trigger_venomous_wounds( const action_state_t* );
  void trigger_vicious_venoms( const action_state_t* state, rogue_attack_t* action );
  void trigger_blade_flurry( const action_state_t* );
  void trigger_ruthlessness_cp( const action_state_t* );
  void trigger_combo_point_gain( int, gain_t* gain = nullptr );
  void trigger_alacrity( const action_state_t* );
  void trigger_deepening_shadows( const action_state_t* );
  void trigger_shadow_techniques( const action_state_t* );
  void trigger_weaponmaster( const action_state_t*, rogue_attack_t* action );
  void trigger_opportunity( const action_state_t*, rogue_attack_t* action, double modifier = 1.0 );
  void trigger_restless_blades( const action_state_t* );
  void trigger_hand_of_fate( const action_state_t*, bool biased = false, bool inevitable = false );
  void execute_fatebound_coinflip( const action_state_t* state, fatebound_t::coinflip_e result );
  void trigger_fate_intertwined( const action_state_t* );
  void trigger_relentless_strikes( const action_state_t* );
  void trigger_blindside( const action_state_t* );
  void trigger_shadow_blades_attack( const action_state_t* );
  void trigger_sting_like_a_bee( const action_state_t* state );
  void trigger_find_weakness( const action_state_t* state, timespan_t duration = timespan_t::min() );
  void trigger_master_of_shadows();
  void trigger_dashing_scoundrel( const action_state_t* state );
  void trigger_count_the_odds( const action_state_t* state, proc_t* source_proc );
  void trigger_keep_it_rolling();
  void trigger_flagellation( const action_state_t* state );
  void trigger_perforated_veins( const action_state_t* state );
  void trigger_inevitability( const action_state_t* state );
  void trigger_lingering_shadow( const action_state_t* state );
  void trigger_danse_macabre( const action_state_t* state );
  void trigger_sanguine_blades( const action_state_t* state, rogue_attack_t* action );
  void trigger_caustic_spatter( const action_state_t* state );
  void trigger_caustic_spatter_debuff( const action_state_t* state );
  void trigger_shadowcraft( const action_state_t* state );
  void trigger_cut_to_the_chase( const action_state_t* state );
  void trigger_cloud_cover( const action_state_t* state );
  void trigger_deathstalkers_mark( const action_state_t* state );
  bool trigger_deathstalkers_mark_debuff( const action_state_t* state, bool from_darkest_night = false );
  void trigger_unseen_blade( const action_state_t* state );
  void trigger_nimble_flurry( const action_state_t* state );
  void trigger_supercharger();
  void trigger_echoing_reprimand( const action_state_t* state );
  void trigger_tww1_assassination_set_bonus( const action_state_t* state );
  void trigger_tww1_outlaw_set_bonus( const action_state_t* );
  void trigger_tww2_set_bonus_removal();

  // General Methods ==========================================================

  void update_ready( timespan_t cd_duration = timespan_t::min() ) override
  {
    if ( secondary_trigger_type != secondary_trigger::NONE )
    {
      cd_duration = timespan_t::zero();
    }

    ab::update_ready( cd_duration );
  }

  timespan_t gcd() const override
  {
    timespan_t t = ab::gcd();

    if ( affected_by.adrenaline_rush_gcd && t != timespan_t::zero() && p()->buffs.adrenaline_rush->check() )
    {
      double reduction_multiplier = std::clamp( ( 1.0 / p()->cache.attack_haste() ) - 1.0, 0.0, 0.25 ) / 0.25;
      t -= ( 200_ms * reduction_multiplier );
    }

    return t;
  }

  virtual double combo_point_da_multiplier( const action_state_t* s ) const
  {
    if ( ab::base_costs[ RESOURCE_COMBO_POINT ] )
      return static_cast<double>( cast_state( s )->get_combo_points() );

    return 1.0;
  }

  double composite_da_multiplier( const action_state_t* state ) const override
  {
    double m = ab::composite_da_multiplier( state );

    m *= combo_point_da_multiplier( state );

    for ( auto damage_buff : direct_damage_buffs )
      m *= damage_buff->is_stacking ? damage_buff->stack_value_direct() : damage_buff->value_direct();
    
    if ( affected_by.mastery_executioner.direct || affected_by.mastery_potent_assassin.direct )
    {
      m *= 1.0 + p()->cache.mastery_value();
    }

    if ( affected_by.deeper_daggers.direct )
    {
      m *= 1.0 + p()->buffs.deeper_daggers->stack_value();
    }

    if ( affected_by.zoldyck_insignia &&
         state->target->health_percentage() < p()->spec.zoldyck_insignia->effectN( 2 ).base_value() )
    {
      m *= 1.0 + p()->spec.zoldyck_insignia->effectN( 1 ).percent();
    }

    if ( affected_by.lethal_dose )
    {
      m *= 1.0 + ( p()->talent.assassination.lethal_dose->effectN( 1 ).percent() *
                   td( state->target )->lethal_dose_count() );
    }

    if ( affected_by.follow_the_blood.direct )
    {
      if ( p()->get_active_dots( td( state->target )->dots.rupture ) >=
           as<unsigned int>( p()->talent.deathstalker.follow_the_blood->effectN( 2 ).base_value() ) )
      {
        m *= 1.0 + p()->talent.deathstalker.follow_the_blood->effectN( 1 ).percent();
      }
    }

    if ( affected_by.darkest_night )
    {
      if ( p()->buffs.darkest_night->up() && cast_state( state )->get_combo_points() >= p()->consume_cp_max() )
      {
        m *= 1.0 + p()->spell.darkest_night_buff->effectN( 2 ).percent();
      }
    }

    if ( affected_by.tww2_subtlety_4pc.direct &&
         p()->buffs.shadow_dance->check() && p()->buffs.tww2_subtlety_2pc->check() )
    {
      m *= 1.0 + ( affected_by.tww2_subtlety_4pc.direct_percent * p()->buffs.tww2_subtlety_2pc->check() );
    }

    return m;
  }

  double composite_ta_multiplier( const action_state_t* state ) const override
  {
    double m = ab::composite_ta_multiplier( state );

    for ( auto damage_buff : periodic_damage_buffs )
      m *= damage_buff->is_stacking ? damage_buff->stack_value_periodic() : damage_buff->value_periodic();

    if ( affected_by.deeper_daggers.periodic )
    {
      m *= 1.0 + p()->buffs.deeper_daggers->stack_value();
    }

    if ( affected_by.mastery_executioner.periodic )
    {
      m *= 1.0 + p()->cache.mastery() * p()->mastery.executioner->effectN( 2 ).mastery_value();
    }

    if ( affected_by.mastery_potent_assassin.periodic )
    {
      m *= 1.0 + p()->cache.mastery() * p()->mastery.potent_assassin->effectN( 2 ).mastery_value();
    }

    if ( affected_by.zoldyck_insignia &&
         state->target->health_percentage() < p()->spec.zoldyck_insignia->effectN( 2 ).base_value() )
    {
      m *= 1.0 + p()->spec.zoldyck_insignia->effectN( 1 ).percent();
    }

    if ( affected_by.lethal_dose )
    {
      m *= 1.0 + ( p()->talent.assassination.lethal_dose->effectN( 1 ).percent() *
                   td( state->target )->lethal_dose_count() );
    }

    if ( affected_by.follow_the_blood.periodic )
    {
      if ( p()->get_active_dots( td( state->target )->dots.rupture ) >=
           as<unsigned int>( p()->talent.deathstalker.follow_the_blood->effectN( 2 ).base_value() ) )
      {
        m *= 1.0 + p()->talent.deathstalker.follow_the_blood->effectN( 1 ).percent();
      }
    }

    if ( affected_by.tww2_subtlety_4pc.periodic &&
         p()->buffs.shadow_dance->check() && p()->buffs.tww2_subtlety_2pc->check() )
    {
      m *= 1.0 + ( affected_by.tww2_subtlety_4pc.periodic_percent * p()->buffs.tww2_subtlety_2pc->check() );
    }

    return m;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = ab::composite_target_multiplier( target );

    if ( affected_by.improved_shiv )
    {
      m *= td( target )->debuffs.shiv->value_direct();
    }

    if ( affected_by.maim_mangle && td( target )->dots.garrote->is_ticking() )
    {
      m *= 1.0 + p()->talent.assassination.systemic_failure->effectN( 1 ).percent();
    }

    if ( affected_by.deathmark )
    {
      m *= td( target )->debuffs.deathmark->value_direct();
    }

    if ( affected_by.ghostly_strike )
    {
      m *= 1.0 + td( target )->debuffs.ghostly_strike->stack_value();
    }

    if ( affected_by.fazed_damage )
    {
      m *= td( target )->debuffs.fazed->value_direct();
    }

    return m;
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = ab::composite_target_crit_chance( target );

    if ( affected_by.fazed_crit_chance && td( target )->debuffs.fazed->check() )
    {
      c += td( target )->debuffs.fazed->value_crit_chance();
    }

    return c;
  }

  double composite_crit_chance() const override
  {
    double c = ab::composite_crit_chance();

    for ( auto crit_chance_buff : crit_chance_buffs )
      c += crit_chance_buff->stack_value_crit_chance();

    if ( affected_by.dashing_scoundrel && p()->buffs.envenom->check() )
    {
      c += p()->spec.dashing_scoundrel->effectN( 1 ).percent() * p()->buffs.envenom->check();
    }

    if ( affected_by.darkest_night_crit && p()->buffs.darkest_night->up() )
    {
      if ( ab::pre_execute_state && cast_state( ab::pre_execute_state )->get_combo_points() >= p()->consume_cp_max() )
      {
        c += 1.0 + p()->spell.darkest_night_buff->effectN( 4 ).percent();
      }
      else if ( p()->current_effective_cp( true ) >= p()->consume_cp_max() )
      {
        c += 1.0 + p()->spell.darkest_night_buff->effectN( 4 ).percent();
      }
    }

    return c;
  }

  double composite_crit_damage_bonus_multiplier() const override
  {
    double cm = ab::composite_crit_damage_bonus_multiplier();

    if ( affected_by.momentum_of_despair && p()->buffs.momentum_of_despair->check() )
    {
      cm *= 1.0 + p()->spell.momentum_of_despair_buff->effectN( 2 ).percent();
    }

    return cm;
  }

  double composite_target_crit_damage_bonus_multiplier( player_t* target )
