-- Event dragons are combat actors; their copied quest menus are not available.
UPDATE `creature_template` SET `gossip_menu_id` = 0
WHERE `ScriptName` = 'npc_era_aq_dragon' AND `npcflag` = 0
  AND ((`entry` = 9500815 AND `gossip_menu_id` = 6795)
    OR (`entry` = 9500816 AND `gossip_menu_id` = 6793)
    OR (`entry` = 9500817 AND `gossip_menu_id` = 6794));
