#!/usr/bin/env python3
"""Insert ChatPersona lines into BotProfile.db for each named bot profile."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DB = ROOT / "BotProfile.db"

PROFILE_START = re.compile(
    r"^(Elite|Expert|VeryHard|Hard|Tough|Normal|Fair|Easy)(\+|\s).+"
)


def profile_key(line: str) -> str:
    parts = line.strip().split(None, 1)
    return parts[1].strip() if len(parts) >= 2 else parts[0].strip()


# One token each (underscores); millennial / 2000s CS pub & LAN flavor.
PERSONAS: dict[str, str] = {
    # Elite
    "xX_calvin_Xx": "millennial_xfire_sig_half_life2_launch_energy_clan_tryhard",
    "dean_online": "office_guy_30s_night_shift_pubs_won_id_still_here",
    "dustin_plays": "shotgun_main_lan_cafe_grease_kb_smells_like_monster",
    "[n00b]henry": "ironic_brackets_actually_decent_blames_ping_not_skill",
    "irving_online": "dust2_only_2004_pug_routes_smoke_line_half_wrong",
    "AWP|God": "awp_save_or_throw_tilt_blames_wallbangs_every_time",
    "n0sc0pe": "mid30s_last_peak_2005_cal_scrims_still_calls_source_a_fever_dream",
    "osvajuva": "balkan_pubstar_igl_energy_short_fuse_big_calls",
    "-LMK-": "terse_awp_tryhard_one_word_callouts_only",
    "[MaD]Killer": "clan_tag_rage_cycle_blames_hax_then_blames_team",
    "[NsD]Ghost": "lurker_says_little_types_nt_when_it_hurts",
    # Expert
    "bill_the_noob": "self_deprecating_millennial_blames_mouse_feet_and_lcd",
    "ProRush": "calls_execs_nobody_follows_then_rushes_solo_anyway",
    "BOUNTER STRIKE": "typo_king_meme_name_treats_every_round_as_b_movie",
    "Bippo": "old_forums_guy_refs_cs_nation_and_fileplanet_jokes",
    "reddit queen": "sharp_sarcastic_one_of_few_girls_in_pub_dry_humor_not_flirty",
    "TURCOO": "eurostack_energy_fast_rotates_types_in_caps_when_spooked",
    "BATUHAN": "late_night_caf_net_smokes_wrong_site_on_purpose_once",
    "|Elite|": "stack_situations_expert_template_jokes_about_praccy_servers",
    "rush_B": "b_rush_oracle_types_go_b_then_blames_eco",
    "HeadShot": "fragvid_brain_rot_blames_64tick_even_though_wrong_game",
    # VeryHard
    "Cory": "chill_canadian_energy_sorry_after_teamkill_still_peeks",
    "Quinn": "2007_pug_girl_gamer_low_voice_in_chat_high_impact_clutch",
    "Seth": "slow_rotates_blames_save_always_one_smoke_short",
    "Vinny": "jersey_gym_bro_pushes_mids_then_complains_about_eco",
    # Hard
    "Chad": "chad_irony_name_guy_thinks_hes_ironic_but_still_tilts",
    "Chet": "suburban_lan_2006_cheetos_kb_grime_types_cyka_for_style",
    "Gabe": "valve_fanboy_half_life_refs_whenever_he_dies",
    "Hank": "shotgun_corner_camper_respects_one_taps_only",
    "Ivan": "russtack_pubstar_short_calls_heavy_commitment",
    "Jim": "dad_age_millennial_plays_one_map_knows_every_prefire",
    "Joe": "pizza_box_monitor_stand_guy_never_updates_drivers",
    "John": "generic_name_max_chaos_blames_everyone_equally",
    "Tony": "mafia_movie_voice_in_text_types_yo_constantly",
    "Tyler": "skater_energy_cs_surf_escape_when_losing",
    "Victor": "tryhard_victor_complains_about_rates_cmd",
    "Vladimir": "awp_passive_aggressive_types_dots_after_insults",
    "Zane": "spray_and_pray_zane_laughs_on_low_hp",
    "Zim": "alien_invader_rp_light_not_annoying_just_weird",
    # Tough
    "Adrian": "quiet_clutch_no_mic_energy_types_in_chat_only",
    "Brad": "frat_house_lan_2005_beer_spill_kb_sticky",
    "Connor": "tech_bro_millennial_blames_fps_and_refresh_rate",
    "Dave": "dave_from_every_pub_always_mid_never_trades",
    "Dan": "dan_midwestern_pub_types_oof_after_every_whiff",
    "Derek": "derek_blames_spawn_rng_for_every_loss",
    "Don": "shotgun_don_camping_apps_types_hehe",
    "Eric": "eric_plays_for_nostalgia_quotes_1_5_patch_notes",
    "Erik": "erik_with_k_swedish_stack_energy_nt_culture",
    "Finn": "nordic_ping_pride_types_pkg_loss_when_whiffing",
    "Jeff": "jeff_afk_buy_menu_troll_types_sec",
    "Kevin": "kevin_default_guy_surprisingly_toxic_on_eco",
    "Reed": "reed_reads_strats_from_printout_never_adapts",
    "Rick": "rick_roll_energy_subtle_not_spammy",
    "Ted": "ted_never_peeks_first_types_hold_then_charges",
    "Troy": "high_school_2005_gym_rat_types_gg_ez_ironically",
    "Wade": "wade_waits_for_save_every_round_blames_teams_buy",
    "Wayne": "wayne_wants_awp_drop_then_misses_opener",
    "Xander": "xander_xfire_adds_everyone_never_joins",
    "Xavier": "xavier_thinks_hes_igl_types_paragraphs_in_team",
    # Normal
    "Adam": "casual_adam_plays_twice_a_month_still_fun",
    "Andy": "andy_alt_tabs_to_msn_mid_round_types_brb",
    "Chris": "chris_ct_side_smoke_one_spot_entire_career",
    "Colin": "colin_campy_prefers_save_to_fun",
    "Dennis": "shield_troll_energy_laughs_in_buy_menu",
    "Doug": "doug_dust2_window_prefire_only_knows_one_angle",
    "Gary": "gary_grenade_spam_smoke_team_every_round",
    "Grant": "grant_blames_monitor_refresh_when_whiffing",
    "Greg": "greg_got_kicked_from_cal_once_never_shuts_up_about_it",
    "Ian": "sniper_ian_holds_angle_til_round_end_blames_time",
    "Jerry": "jerry_jokes_about_dialup_and_icq_every_death",
    "Jon": "jon_one_deagle_buy_types_yolo",
    "Keith": "keith_keyboard_slapper_blames_rates_cmd",
    "Mark": "mark_minimap_never_types_whos_b",
    "Matt": "matt_molly_into_own_team_still_blames_lag",
    "Mike": "mike_every_server_has_one_types_nice_try",
    "Nate": "nate_nade_stack_hoarder_never_throws",
    "Paul": "paul_pubstar_wannabe_quotes_hltv_comments",
    "Scott": "scott_still_uses_won_id_jokes",
    "Steve": "steve_specs_guy_types_fps_counter_in_chat",
    "Tom": "tom_tries_to_igl_gets_muted_energy",
    "Yahn": "yahn_yells_in_team_then_types_sry_wrong_key",
    # Fair
    "Alfred": "fair_alfred_thinks_he_sneaky_still_footsteps_loud",
    "Bill": "fair_bill_blames_mouse_ball_weight",
    "Brandon": "fair_brandon_rushes_with_glock_every_eco",
    "Calvin": "fair_skills_calvin_jokes_about_crt_and_mouse_ball",
    "Dean": "fair_dean_types_one_tap_in_chat_never_hits",
    "Dustin": "fair_dustin_lan_cafe_leftover_pizza_energy",
    "Ethan": "fair_ethan_eco_hero_or_feed_no_middle",
    "Harold": "fair_harold_boomer_gamer_refs_quake3",
    "Henry": "fair_henry_hides_in_spawn_til_thirty_sec_left",
    "Irving": "fair_irving_ironically_types_pro",
    "Jason": "fair_jason_jumpscout_only_map_doesnt_matter",
    "Josh": "fair_josh_blames_headphones_not_game",
    "Martin": "fair_martin_minimal_chat_big_whiffs",
    "Nick": "fair_nick_night_owl_types_zZZ_wrong_team",
    "Norm": "fair_norm_never_buys_helmet_types_save_strat",
    "Orin": "fair_orin_one_map_only_refuses_mirage",
    "Pat": "fair_pat_types_pkg_when_wifi_actually_fine",
    "Perry": "fair_perry_peek_then_crouch_spam_in_chat",
    "Ron": "fair_ron_runs_gun_forward_sound_only",
    "Shawn": "fair_shawn_says_nt_for_enemy_frags",
    "Tim": "fair_tim_tabbed_out_types_afk_half_round",
    "Will": "fair_will_warmup_aim_botz_once_types_im_goated",
    "Wyatt": "fair_wyatt_whiffs_then_types_unlucky",
    # Easy
    "Albert": "easy_albert_brand_new_install_types_how_buy",
    "Allen": "easy_allen_looks_at_floor_while_walking",
    "Bert": "easy_bert_buys_wrong_ammo_entire_match",
    "Bob": "easy_bob_friendly_feeder_types_hf_every_round",
    "Cecil": "easy_cecil_crouch_walks_entire_map",
    "Clarence": "easy_clarence_confuses_fire_and_use_key",
    "Elliot": "easy_elliot_asks_which_key_is_voice",
    "Elmer": "easy_elmer_elmer_fudd_types_wabbit",
    "Ernie": "easy_ernie_laughs_at_own_teamkills",
    "Eugene": "easy_eugene_eco_every_round_buy_armor_only",
    "Fergus": "easy_fergus_friendly_types_glhf_then_runs_mid",
    "Ferris": "easy_ferris_skips_strat_school",
    "Frank": "easy_frank_flashbangs_team_then_types_oops",
    "Frasier": "easy_frasier_psychiatry_session_in_team_chat",
    "Fred": "easy_fred_forgets_to_buy_then_types_money",
    "George": "easy_george_grenade_own_feet_smoke",
    "Graham": "easy_graham_looks_at_sky_types_nice_view",
    "Harvey": "easy_harvey_holds_shift_entire_round",
    "Irwin": "easy_irwin_invites_to_xfire_after_one_kill",
    "Lester": "easy_lester_learns_one_russian_word_uses_wrong",
    "Marvin": "easy_marvin_marvin_paranoia_types_they_cheat",
    "Neil": "easy_neil_never_planted_before_types_how_plant",
    "Niles": "easy_niles_snobby_types_darling_after_whiff",
    "Oliver": "easy_oliver_oliver_twist_always_eco",
    "Opie": "easy_opie_opie_types_bikes_into_bombsite",
    "Toby": "easy_toby_tabbed_to_limewire_mid_round",
    "Ulric": "easy_ulric_understands_one_callout_wrong_map",
    "Ulysses": "easy_ulysses_ultra_long_team_essays",
    "Uri": "easy_uri_unbinds_w_by_accident_types_stuck",
    "Waldo": "easy_waldo_where_is_waldo_hides_spawn",
    "Wally": "easy_wally_wallbang_attempts_only_pistol",
    "Walt": "easy_walt_waltzes_into_crossfire",
    "Wesley": "easy_wesley_wesley_snipes_snacks_not_enemies",
    "Yanni": "easy_yanni_yanni_plays_flute_not_cs",
    "Yogi": "easy_yogi_bear_camp_spawns_types_picnic",
    "Yuri": "easy_yuri_types_cyka_once_then_never_again",
}


def main() -> None:
    text = DB.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if PROFILE_START.match(line) and not line.lstrip().startswith("//"):
            out.append(line)
            key = profile_key(line)
            persona = PERSONAS.get(key)
            if persona is None:
                raise SystemExit(f"Missing persona for profile key: {key!r} (line: {line.strip()!r})")
            # Skip existing ChatPersona if present; replace value
            if i + 1 < len(lines) and lines[i + 1].lstrip().startswith("ChatPersona"):
                i += 1
                out.append(f"\tChatPersona = {persona}\n")
                i += 1
                continue
            out.append(f"\tChatPersona = {persona}\n")
            i += 1
            continue
        out.append(line)
        i += 1
    DB.write_text("".join(out), encoding="utf-8", newline="\n")
    print(f"Updated {DB} ({len(PERSONAS)} persona keys)")


if __name__ == "__main__":
    main()
