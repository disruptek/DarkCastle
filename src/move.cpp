/************************************************************************
| $Id: move.cpp,v 1.8 2002/07/24 19:01:12 pirahna Exp $
| move.C
| Movement commands and stuff.
*/
#include <character.h>
#include <affect.h>
#include <room.h>
#include <levels.h>
#include <utility.h>
#include <player.h>
#include <fight.h>
#include <mobile.h>
#include <interp.h>
#include <spells.h>
#include <handler.h>
#include <db.h>
#include <obj.h>
#include <connect.h>
#include <act.h>
#include <race.h> // RACE_FISH
#include <clan.h> // clan_room_data
#include <string.h>
#include <returnvals.h>
#include <game_portal.h>

#ifdef LEAK_CHECK
#include <dmalloc.h>
#endif

extern struct obj_data *object_list;
extern struct index_data *obj_index;
extern struct zone_data *zone_table;
extern CWorld world;
 
extern char *dirs[];
extern int movement_loss[];

int ambush(CHAR_DATA *ch); // class/cl_ranger.C


int move_player(char_data * ch, int room)
{
  int retval;

  retval = move_char(ch, room);

  if(!IS_SET(retval, eSUCCESS))
  {
    retval = move_char(ch, real_room(START_ROOM));
    if(!IS_SET(retval, eSUCCESS))
      log("Failed to move ch to start room.  move_player_home_nofail", IMMORTAL, LOG_BUG);
  }

  return retval;
}

void move_player_home(CHAR_DATA *victim)
{
  int was_in = victim->in_room;
  int found = 0;
  struct clan_data * clan = NULL;
  struct clan_room_data * room = NULL;

  struct clan_data * get_clan(CHAR_DATA *ch);

  // check for homes that don't exist
  if(real_room(GET_HOME(victim) < 1))
    GET_HOME(victim) = START_ROOM;

  // next four lines ping-pong people from meta to tavern to help lessen spam
  if (world[was_in].number == GET_HOME(victim) && GET_HOME(victim) == START_ROOM)
    move_player(victim, real_room(SECOND_START_ROOM));
  else if (world[was_in].number == GET_HOME(victim))
    move_player(victim, real_room(START_ROOM));

  // now we check if they are recalling into a clan room
  else if (!IS_SET(world[real_room(GET_HOME(victim))].room_flags, CLAN_ROOM))
      // nope, let's go
    move_player(victim, real_room(GET_HOME(victim)));
  else { // clanroom else 
    if(!victim->clan || !(clan = get_clan(victim))) {
      send_to_char("The gods frown on you, and reset your home.\r\n", victim);
      GET_HOME(victim) = START_ROOM;
      move_player(victim, real_room(GET_HOME(victim)));
    } else {
      for(room = clan->rooms; room; room = room->next)
        if(room->room_number == GET_HOME(victim))
          found = 1;
        if(!found) {
          send_to_char("The gods frown on you, and reset your home.\r\n", victim);
          GET_HOME(victim) = START_ROOM;
        }
        move_player(victim, real_room(GET_HOME(victim)));
    }
  } // of clan room else 
}

// Rewritten 9/1/96
// -Sadus
void record_track_data(CHAR_DATA *ch, int cmd)
{
  room_track_data * newScent;
  
  // Rangers outdoors leave no tracks
  if(world[ch->in_room].sector_type != SECT_INSIDE &&
     GET_CLASS(ch) == CLASS_RANGER)
    return;
    
  // If we just used a scent killer like catstink, don't leave tracks
  // on our next move
  if(IS_SET(ch->affected_by2, AFF_UTILITY)) {
    REMOVE_BIT(ch->affected_by2, AFF_UTILITY);
    return;
  }

  if(world[ch->in_room].sector_type == SECT_WATER_SWIM ||
     world[ch->in_room].sector_type == SECT_WATER_NOSWIM )
    return;

#ifdef LEAK_CHECK
  newScent = (room_track_data *)calloc(1, sizeof(struct room_track_data));
#else
  newScent = (room_track_data *)dc_alloc(1, sizeof(struct room_track_data));
#endif
  newScent->direction = cmd;
  newScent->weight    = (int)ch->weight;
  newScent->race      = (int)ch->race;
  newScent->sex       = (int)ch->sex;
  newScent->condition = ((GET_HIT(ch) * 100) / 
                      (GET_MAX_HIT(ch) == 0 ? 100 : GET_MAX_HIT(ch)));
  newScent->next      = NULL; // just in case
  newScent->previous  = NULL; // just in case

  if(IS_NPC(ch))
    newScent->trackee = GET_NAME(ch);
  else
    newScent->trackee = str_hsh(GET_NAME(ch));

  world[ch->in_room].AddTrackItem(newScent);
  
  return;
}

int do_unstable(CHAR_DATA *ch) 
{
    if(IS_NPC(ch))
        return eFAILURE;

    short chance = number(0, 30);
 
    if(IS_AFFECTED(ch, AFF_FLYING))
        return eFAILURE;

    if(GET_DEX(ch) > chance) {
        act("You dextrously maintain your footing.", ch, 0, 0, TO_CHAR, 0);
        return eFAILURE;
    }
    act("$n looses his footing on the unstable ground.", ch, 0, 0, TO_ROOM, 0);
    act("You fall flat on your ass.", ch, 0, 0, TO_CHAR, 0);
    GET_HIT(ch) -= GET_MAX_HIT(ch) / 12;
    GET_POS(ch) = POSITION_SITTING;

    if((GET_HIT(ch) <= 0)) {
        act("$n's frail body snaps in half as $e is buffeted about the room.", ch, 0, 0, TO_ROOM, 0);
        act("You feel your back snap painfully and all goes dark...", ch, 0, 0, TO_CHAR, 0);
        send_to_char("You have been KILLED!\n\r", ch);
        fight_kill(ch, ch, TYPE_CHOOSE);
        return eSUCCESS|eCH_DIED;
    }
    return eSUCCESS;
}


int do_fall(CHAR_DATA *ch, short dir)
{
  int target;
  char damage[MAX_STRING_LENGTH];
  int retval;

  if(IS_AFFECTED(ch, AFF_FLYING))
    return eFAILURE;

  // Don't effect mobs
  if(IS_NPC(ch)) {
    act("$n's clings to the terrain around $m and avoids falling.", ch, 0, 0, TO_ROOM, 0);
    return eFAILURE;
  }

  if(!CAN_GO(ch, dir)) 
    return eFAILURE; // no exit

  target = world[ch->in_room].dir_option[dir]->to_room;
  if(IS_SET(world[target].room_flags, IMP_ONLY) && GET_LEVEL(ch) < IMP)
    return eFAILURE;  // not gonna do it

  sprintf(damage,"%s falls from %d and sustains %d damage.",
          GET_NAME(ch), world[ch->in_room].number, (GET_MAX_HIT(ch)/6)); 
  log(damage, IMMORTAL, LOG_MORTAL);

  switch(dir) {
      case 0:
          act("$n rolls out to the north.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You tumble to the north...\n\r", ch);
          break;
      case 1:
          act("$n rolls out to the east.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You tumble to the east...\n\r", ch);
          break;
      case 2:
          act("$n rolls out to the south.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You tumble to the south...\n\r", ch);
          break;
      case 3:
          act("$n rolls out to the west.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You tumble to the west...\n\r", ch);
          break;
      case 4:
          act("$n is launched upwards.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You are launched into the air...\n\r", ch);
          break;

      case 5:
          act("$n falls through the room.", ch, 0, 0, TO_ROOM, 0);
          send_to_char("You fall...\n\r", ch);
          break;
      default:
          log("Default hit in do_fall", IMMORTAL, LOG_MORTAL);
          break;
  }
  retval = move_char(ch, target);
  if(!IS_SET(retval, eSUCCESS)) {
      send_to_char("You are miraculously upheld by divine powers!\n\r", ch);
      return retval;
  }
  do_look(ch, "\0", 15);

  GET_HIT(ch) -= (GET_MAX_HIT(ch)/6);
  update_pos(ch);
  if(GET_POS(ch) == POSITION_DEAD) {
     send_to_char("Luckily the ground breaks your fall.\n\r", ch);
     send_to_char("\n\rYou have been KILLED!\n\r", ch);
     fight_kill(ch, ch, TYPE_CHOOSE); 
     sprintf(damage,"%s fall from %d was lethal and killed them.",
          GET_NAME(ch), world[ch->in_room].number);
     log(damage, IMMORTAL, LOG_MORTAL);
     return eSUCCESS|eCH_DIED;
  }
  if((IS_SET(world[ch->in_room].room_flags, FALL_DOWN) && (dir = 5)) ||
    (IS_SET(world[ch->in_room].room_flags, FALL_UP) && (dir = 4)) ||
    (IS_SET(world[ch->in_room].room_flags, FALL_EAST) && (dir = 1)) ||
    (IS_SET(world[ch->in_room].room_flags, FALL_WEST) && (dir = 3)) ||
    (IS_SET(world[ch->in_room].room_flags, FALL_SOUTH) && (dir = 2)) ||
    (IS_SET(world[ch->in_room].room_flags, FALL_NORTH) && (dir = 0))) {
    return do_fall(ch, dir);
  }
/* Don't think we need this - pir
  else if(GET_POS(ch) == POSITION_DEAD) { 
    send_to_char("SPLAT! You have been KILLED!\n\r", ch);
    fight_kill(ch, ch, TYPE_CHOOSE); 
    return 0;
  }
*/
  return eSUCCESS; // if we're here, we lived.
}

int do_simple_move(CHAR_DATA *ch, int cmd, int following)
/* Assumes, 
    1. That there is no master and no followers.
    2. That the direction exists. 

   Returns :
    standard returnvals
*/
{
    char tmp[80];
    int dir;
    int was_in;
    int need_movement;
    int retval;
    struct obj_data *obj;
    bool has_boat;

// I think this is taken care of in the command interpreter.  Disabling it for now.
// -pir 7/25/01
//    retval = special(ch, cmd+1, "");  /* Check for special routines (North is 1) */
//    if(IS_SET(retval, eSUCCESS) || IS_SET(retval, eCH_DIED))
//	return retval;

    if (IS_AFFECTED(ch, AFF_FLYING))
       need_movement = 1;
    
    else   {
      need_movement = (movement_loss[world[ch->in_room].sector_type] +
        movement_loss[world[world[ch->in_room].dir_option[cmd]->to_room].sector_type])
	/ 2;

      // if I'm trying to go "up" into a "fall down" room, etc.
      // it's OK to go east into a "fall north" room though
      // not ok, if room we're going to is AIR though
      if( !IS_AFFECTED(ch, AFF_FLYING) &&
         (
           ( cmd == 0 && IS_SET( world[world[ch->in_room].dir_option[0]->to_room].room_flags, FALL_SOUTH ) ) ||
           ( cmd == 1 && IS_SET( world[world[ch->in_room].dir_option[1]->to_room].room_flags, FALL_WEST ) ) ||
           ( cmd == 2 && IS_SET( world[world[ch->in_room].dir_option[2]->to_room].room_flags, FALL_NORTH ) ) ||
           ( cmd == 3 && IS_SET( world[world[ch->in_room].dir_option[3]->to_room].room_flags, FALL_EAST ) ) ||
           ( cmd == 4 && IS_SET( world[world[ch->in_room].dir_option[4]->to_room].room_flags, FALL_DOWN ) ) ||
           ( cmd == 5 && IS_SET( world[world[ch->in_room].dir_option[5]->to_room].room_flags, FALL_UP ) ) ||
           world[world[ch->in_room].dir_option[cmd]->to_room].sector_type == SECT_AIR
         )
        )
      {
         send_to_char("You would need to fly to go there!\n\r", ch);
         return eFAILURE;
      }
     
/*  above is a little more effecient since we aren't checking AFF_FLYING every single time.
    Looks nicer too:)
    Added sector_air check on it as well
    7/18/02 - pir
      if( ( 
         (cmd == 0 && !IS_AFFECTED(ch, AFF_FLYING) ) &&
         ( IS_SET( world[world[ch->in_room].dir_option[0]->to_room].room_flags, FALL_SOUTH ) )) ||
         ((cmd == 1 && !IS_AFFECTED(ch, AFF_FLYING)) &&
         (IS_SET(world[world[ch->in_room].dir_option[1]->to_room].room_flags, FALL_WEST))) ||
         ((cmd == 2 && !IS_AFFECTED(ch, AFF_FLYING)) &&
         (IS_SET(world[world[ch->in_room].dir_option[2]->to_room].room_flags, FALL_NORTH))) ||
         ((cmd == 3 && !IS_AFFECTED(ch, AFF_FLYING)) &&
         (IS_SET(world[world[ch->in_room].dir_option[3]->to_room].room_flags, FALL_EAST))) ||
         ((cmd == 4 && !IS_AFFECTED(ch, AFF_FLYING)) &&
         (IS_SET(world[world[ch->in_room].dir_option[4]->to_room].room_flags, FALL_DOWN))) ||
         ((cmd == 5 && !IS_AFFECTED(ch, AFF_FLYING)) &&
         (IS_SET(world[world[ch->in_room].dir_option[5]->to_room].room_flags, FALL_UP)))) {
         send_to_char("You would need to fly to go there!\n\r", ch);
         return eFAILURE;
      }
*/

      // fly down't work over water 
      if ((world[ch->in_room].sector_type == SECT_WATER_NOSWIM) ||
          (world[world[ch->in_room].dir_option[cmd]->to_room].sector_type == SECT_WATER_NOSWIM)) {
	 has_boat = FALSE;
	 // See if char is carrying a boat
	 for (obj = ch->carrying; obj; obj = obj->next_content)
	     if (obj->obj_flags.type_flag == ITEM_BOAT)
		has_boat = TRUE;
         // See if char is wearing a boat (boat ring, etc)
         if(!has_boat)
           for(int x = 0; x <= MAX_WEAR; x++)
             if(ch->equipment[x])
               if(ch->equipment[x]->obj_flags.type_flag == ITEM_BOAT)
                 has_boat = TRUE;

	 if (!has_boat && !IS_AFFECTED(ch, AFF_FLYING) && 
             GET_RACE(ch) != RACE_FISH && GET_RACE(ch) != RACE_SLIME) 
         {
	    send_to_char("You need a boat to go there.\n\r", ch);
	    return eFAILURE;
	 }
      }

      if(!IS_NPC(ch) &&
         world[world[ch->in_room].dir_option[cmd]->to_room].sector_type == SECT_UNDERWATER &&
         !affected_by_spell(ch, SPELL_WATER_BREATHING)
        )
      {
         send_to_char("Underwater?!\r\n", ch);
         return eFAILURE;
      }

      if(world[world[ch->in_room].dir_option[cmd]->to_room].sector_type != SECT_WATER_NOSWIM &&
         world[world[ch->in_room].dir_option[cmd]->to_room].sector_type != SECT_WATER_SWIM &&
         world[world[ch->in_room].dir_option[cmd]->to_room].sector_type != SECT_UNDERWATER)
      {  // It's NOT a water room and we don't have fly
         if(GET_RACE(ch) == RACE_FISH)
         {
            send_to_char("You can't swim around outside water without being able to fly!\r\n", ch);
            act("$n flops around in a futile attempt to move out of water.", ch, 0, 0, TO_ROOM, INVIS_NULL);
            return eFAILURE;
         }
      }
    } // else if !FLYING

    if(IS_SET(world[world[ch->in_room].dir_option[cmd]->to_room].room_flags,
       IMP_ONLY) && GET_LEVEL(ch) < IMP) {
      send_to_char("No.\n\r", ch);
      return eFAILURE;
    } 

    // if I'm STAY_NO_TOWN, don't enter a ZONE_IS_TOWN zone no matter what
    if(IS_NPC(ch) && 
       IS_SET(ch->mobdata->actflags, ACT_STAY_NO_TOWN) && 
       IS_SET(zone_table[world[world[ch->in_room].dir_option[cmd]->to_room].zone].zone_flags, ZONE_IS_TOWN))
      return eFAILURE;

    if(GET_MOVE(ch) < need_movement && !IS_NPC(ch)) {
	if(!following)
	    send_to_char("You are too exhausted.\n\r",ch);
	else
	    send_to_char("You are too exhausted to follow.\n\r",ch);

	return eFAILURE;
    }

    // If they were hit with a lullaby, go ahead and clear it out
    if(IS_SET(ch->affected_by2, AFF_NO_FLEE)) 
    {
      if(affected_by_spell(ch, SPELL_IRON_ROOTS)) {
        send_to_char("The roots bracing your legs keep you from moving!\r\n", ch);
        return eFAILURE;
      }
      else {
        REMOVE_BIT(ch->affected_by2, AFF_NO_FLEE);
        send_to_char("The travel wakes you up some, and clears the drowsiness from your legs.\r\n", ch);
      }
    }

    if(GET_LEVEL(ch) < IMMORTAL && !IS_NPC(ch))
	GET_MOVE(ch) -= need_movement;

    if(!IS_AFFECTED(ch, AFF_SNEAK) && !IS_AFFECTED2(ch, AFF_FOREST_MELD)) 
    {
      sprintf(tmp, "$n leaves %s.", dirs[cmd]);
      act(tmp, ch, 0, 0, TO_ROOM, INVIS_NULL);
    }
    
    // I am affected by sneak or meld...I have a chance of failing
    // every time I move.
    else if(IS_AFFECTED(ch, AFF_SNEAK)) 
    {
      char tmp[100];
      int learned = has_skill(ch, SKILL_SNEAK);
      int percent = number(1, 101); // 101% is a complete failure

      // TODO - when mobs have skills, remove this
      if(IS_MOB(ch))
         learned = 80;
      
      if (percent > learned) {
         sprintf(tmp, "$n leaves %s.", dirs[cmd]);
         act(tmp, ch, 0, 0, TO_ROOM, INVIS_NULL|STAYHIDE);
      }
      else {
        sprintf(tmp, "$n sneaks %s.", dirs[cmd]);
        act(tmp, ch, 0, 0, TO_ROOM, GODS);
      }
    }
    else { // AFF_FOREST MELD
      if(world[world[ch->in_room].dir_option[cmd]->to_room].sector_type !=
         SECT_FOREST)
      {
        sprintf(tmp, "$n leaves %s.", dirs[cmd]);
        REMOVE_BIT(ch->affected_by2, AFF_FOREST_MELD);
        send_to_char("You detach yourself from the forest.\r\n", ch);
        act(tmp, ch, 0, 0, TO_ROOM, INVIS_NULL);
      } 
      else {
          sprintf(tmp, "$n sneaks %s.", dirs[cmd]); 
          act(tmp, ch, 0, 0, TO_ROOM, GODS);  
      }
    } // AFF_FOREST_MELD

    was_in = ch->in_room;
    record_track_data(ch, cmd);

    retval = move_char(ch, world[was_in].dir_option[cmd]->to_room);
    if(!IS_SET(retval, eSUCCESS)) {
      send_to_char("You fail to move.\n\r", ch);
      return retval;
    }

    if (ch->fighting) {
       char_data * chaser = ch->fighting;
       if (IS_NPC(ch)) {
          add_memory(ch, GET_NAME(chaser), 'f');
          remove_memory(ch, 'h');
       }
       if (IS_NPC(chaser))
          add_memory(chaser, GET_NAME(ch), 't');

       if (chaser->fighting == ch)  
          stop_fighting(chaser);

       stop_fighting(ch);
 
       // This might be a bad idea...cause track calls move, which calls track, which...              
       if (IS_NPC(chaser)) {
          retval = do_track(chaser, chaser->mobdata->hatred, 9);
          if (SOMEONE_DIED(retval))
            return retval;
       }         
    }
    if (IS_AFFECTED(ch, AFF_SNEAK) ||
        (IS_AFFECTED2(ch, AFF_FOREST_MELD) &&
         world[ch->in_room].sector_type == SECT_FOREST)
       )
      act("$n sneaks into the room.", ch, 0, 0, TO_ROOM, GODS);
    else
	act("$n has arrived.", ch, 0,0, TO_ROOM, INVIS_NULL); 

    do_look(ch, "\0",15);

    if((IS_SET(world[ch->in_room].room_flags, FALL_NORTH) && (dir = 0)) ||
      (IS_SET(world[ch->in_room].room_flags, FALL_DOWN) && (dir = 5)) ||
      (IS_SET(world[ch->in_room].room_flags, FALL_UP) && (dir = 4)) ||
      (IS_SET(world[ch->in_room].room_flags, FALL_EAST) && (dir = 1)) ||
      (IS_SET(world[ch->in_room].room_flags, FALL_WEST) && (dir = 3)) ||
      (IS_SET(world[ch->in_room].room_flags, FALL_SOUTH) && (dir = 2))) {

      retval = do_fall(ch, dir);
      if(SOMEONE_DIED(retval))
        return eSUCCESS|eCH_DIED;
    }

    if(IS_SET(world[ch->in_room].room_flags, UNSTABLE)) {
      retval = do_unstable(ch);
      if(SOMEONE_DIED(retval))
         return eSUCCESS|eCH_DIED; 
    }

    // let our mobs know they're here
    retval = mprog_entry_trigger( ch );
    // if either one died, we don't want to continue
    if(SOMEONE_DIED(retval))
      return retval|eSUCCESS;
    retval = mprog_greet_trigger( ch );
    if(SOMEONE_DIED(retval))
      return retval|eSUCCESS;

    return eSUCCESS;
}


//   Returns :
//    standard retvals
int attempt_move(CHAR_DATA *ch, int cmd, int is_retreat = 0)
{
  char tmp[80];
  int  return_val;
  int  was_in = ch->in_room;
  
  struct follow_type *k, *next_dude;

  --cmd;

  if(!world[ch->in_room].dir_option[cmd]) {
    send_to_char("You can't go that way.\n\r", ch);
    return eFAILURE;
  }

  if(IS_SET(EXIT(ch, cmd)->exit_info, EX_CLOSED)) {
    if(IS_SET(EXIT(ch, cmd)->exit_info, EX_HIDDEN))
      send_to_char("You can't go that way.\n\r", ch);
    else if(EXIT(ch, cmd)->keyword)
      csendf(ch, "The %s seems to be closed.\n\r",
             fname(EXIT(ch, cmd)->keyword));
    else 
      send_to_char("It seems to be closed.\n\r", ch);
    return eFAILURE;
  }

  if(EXIT(ch, cmd)->to_room == NOWHERE) { 
    send_to_char("Alas, you can't go that way.\n\r", ch);
    return eFAILURE;
  }

  if(!ch->followers && !ch->master) { 
    return_val = do_simple_move(ch, cmd, FALSE);
    if(!IS_AFFECTED(ch, AFF_SNEAK) && 
        IS_SET(return_val, eSUCCESS) &&
       !IS_SET(return_val, eCH_DIED))
      return ambush(ch);
    return return_val;
  }

  if(IS_AFFECTED(ch, AFF_CHARM) && (ch->master) && 
     (ch->in_room == ch->master->in_room)) {
    send_to_char("You are unable to abandon your master.\n\r", ch); 
    act("$n trembles as $s mind attempts to free itself from its magical bondage.", ch, 0, 0, TO_ROOM, 0);
    return eFAILURE;
  }

  return_val = do_simple_move(ch, cmd, TRUE);

  // this may cause problems with leader being ambushed, dying, and group not moving
  // but we have to be careful in case leader was a mob (no longer valid memory)
  if(SOMEONE_DIED(return_val) || !IS_SET(return_val, eSUCCESS))
    return return_val; 

  if(ch->followers)
    for(k = ch->followers; k; k = next_dude) {
       next_dude = k->next;
       if((was_in == k->follower->in_room) &&
          ((is_retreat && GET_POS(k->follower) > POSITION_RESTING) || 
          (GET_POS(k->follower) >= POSITION_STANDING))
	  ) {
         if(CAN_SEE(k->follower, ch))
           sprintf(tmp, "You follow %s.\n\r\n\r", GET_SHORT(ch));
         else
           strcpy(tmp, "You follow someone.\n\r\n\r");
         send_to_char(tmp, k->follower); 
         do_move(k->follower, "", cmd + 1);
       }
    } 

  if(was_in != ch->in_room && !IS_AFFECTED(ch, AFF_SNEAK))
    return_val = ambush(ch);

  if(IS_SET(return_val, eCH_DIED))
    return eSUCCESS|eCH_DIED;

  return eSUCCESS;
}

//   Returns :
//   1 : If success.
//   0 : If fail
//  -1 : If dead.
int do_move(CHAR_DATA *ch, char *argument, int cmd)
{
  return attempt_move(ch, cmd);
}

int do_leave(struct char_data *ch, char *arguement, int cmd)
{
  struct obj_data *k;
  int origination;
  char buf[200];
  int retval;

  for(k = object_list; k; k = k->next) {
   if((k->obj_flags.type_flag == ITEM_PORTAL) &&
     (k->obj_flags.value[1] == 1 || (k->obj_flags.value[1] == 2)) &&
     (k->in_room > -1) &&
     !IS_SET(k->obj_flags.value[3], PORTAL_NO_LEAVE))
     {
     if((k->obj_flags.value[0] == world[ch->in_room].number) ||
        (k->obj_flags.value[2] == world[ch->in_room].zone))
        { 
         send_to_char("You exit the area.\n\r", ch);
         act("$n has left the area.",  ch, 0, 0, TO_ROOM, INVIS_NULL|STAYHIDE);
	 origination = ch->in_room;
	 retval = move_char(ch, real_room(world[k->in_room].number));
         if(!IS_SET(retval, eSUCCESS)) {
	    send_to_char("You attempt to leave, but the door slams in your face!\n\r", ch);
	    act("$n attempts to leave, but can't!", ch, 0, 0, TO_ROOM, INVIS_NULL|STAYHIDE);
	    return eFAILURE;
	 }
	 
         do_look(ch, "", 9);         
         sprintf(buf, "%s walks out of %s.", GET_NAME(ch), k->short_description);
         act(buf, ch, 0, 0, TO_ROOM, INVIS_NULL|STAYHIDE);
         return eSUCCESS;
        }
     } 
  }
  send_to_char("There are no exits around!\n\r", ch);
  return eFAILURE;
}

int do_enter(CHAR_DATA *ch, char *argument, int cmd)
{
  char buf[MAX_STRING_LENGTH];
  int retval;

  CHAR_DATA *sesame;
  OBJ_DATA *portal = NULL;
  if((ch->in_room != -1) || (ch->in_room)) {
     one_argument(argument, buf);
  }
  if(!*buf) {
     send_to_char("Enter what?\n\r", ch);
     return eFAILURE;
  } 
  if((portal = get_obj_in_list_vis(ch, buf, world[ch->in_room].contents)) == NULL) {
     send_to_char("Nothing here by that name.\n\r", ch);
     return eFAILURE;
  }
  if(portal->obj_flags.type_flag != ITEM_PORTAL) {
     send_to_char("You can't enter that!\n\r", ch);
     return eFAILURE;
   }

   if(GET_LEVEL(ch) > IMMORTAL &&
      GET_LEVEL(ch) < DEITY    &&
      IS_SET(world[real_room(portal->obj_flags.value[0])].room_flags, CLAN_ROOM))
   {
      send_to_char("104-'s may NOT go into a clanhall.\r\n", ch);
      act("$n realizes what a moron they are.", ch, 0, 0, TO_ROOM, 0);
      return eFAILURE;
   }
      
   if (IS_NPC(ch) && ch->master) {
      sesame = ch->master;
      if (IS_SET(world[real_room(portal->obj_flags.value[0])].room_flags, CLAN_ROOM)) {
         send_to_char("You are a mob, not a clan member.\n\r", ch);
         act("$n finds $mself unable to enter!", ch, 0, 0, TO_ROOM, 0);
         return eFAILURE;
         }
      }
   else {
      sesame = ch;
      }

   // should probably just combine this with 'if' below it, but i'm lazy
   if(IS_SET(portal->obj_flags.value[3], PORTAL_NO_ENTER)) {
     send_to_char("The portal's destination rebels against you.\r\n", ch);
     act("$n finds $mself unable to enter!", ch, 0, 0, TO_ROOM, 0);
     return eFAILURE;
   }

   if (!IS_MOB(ch) && IS_SET(ch->pcdata->punish, PUNISH_THIEF) &&
       IS_SET(world[real_room(portal->obj_flags.value[0])].room_flags, CLAN_ROOM))
   {
     send_to_char("The portal's destination rebels against you.\r\n", ch);
     act("$n finds $mself unable to enter!", ch, 0, 0, TO_ROOM, 0);
     return eFAILURE;
   }
   if(isname("only", portal->name) && !isname(GET_NAME(sesame), portal->name)) {
      send_to_char("The portal fades when you draw near, then shimmers as you "
                   "withdraw.\n\r", ch);
      return eFAILURE;
   }
   switch(portal->obj_flags.value[1]) {
      case 0: // portal created by portal spell
         send_to_char("You reach out tentatively and touch the portal...\n\r", ch);
         act("$n reaches out to the glowing dimensional portal....", 
              ch, 0, 0, TO_ROOM, 0);
         break;
      case 1:
      case 2:
         sprintf(buf, "You take a bold step towards %s.\n\r", portal->short_description);
         send_to_char(buf, ch);
         sprintf(buf, "%s boldly walks toward %s and disappears.", GET_NAME(ch),
                 portal->short_description);
         act(buf, ch, 0, 0, TO_ROOM, INVIS_NULL|STAYHIDE);
         break;
      case 3:
         send_to_char("You cannot enter that.\n\r", ch);
         return eFAILURE;
      default:
         sprintf(buf, "Someone check value 1 on object %d, returned default on "
                       "do_enter", portal->item_number);
         log(buf, OVERSEER, LOG_BUG);
         return eFAILURE;
   }

   retval = move_char(ch, real_room(portal->obj_flags.value[0]));
   if(SOMEONE_DIED(retval)) 
      return retval;
   if(!IS_SET(retval, eSUCCESS)) {
      send_to_char("You recoil in pain as the portal slams shut!\n\r", ch);
      act("$n recoils in pain as the portal slams shut!", ch, 0, 0, TO_ROOM, 0);
   }
   switch(portal->obj_flags.value[1]) {
      case 0:
         do_look(ch, "", 9);
         WAIT_STATE(ch, PULSE_VIOLENCE);
         send_to_char("\n\rYou are momentarily dazed from the dimensional shift\n\r", ch);
         act("The portal glows brighter for a second as $n appears beside you.", ch, 0, 0, TO_ROOM, 0); 
         break;
      case 1:
      case 2:
         sprintf(buf, "%s has entered %s.", GET_NAME(ch), portal->short_description);
         act(buf, ch, 0, 0, TO_ROOM, STAYHIDE);
         do_look(ch, "", 9);
         break;
      case 3:
         break;
      default:
         break;
   }
   return eSUCCESS;
}

/************************************************************************
| move_char			move.C			IE
*/
int move_char (CHAR_DATA *ch, int dest)
{
  if(!ch) {
    log("NULL character sent to move_char!", OVERSEER, LOG_BUG);
    return eINTERNAL_ERROR;
  }
  
  int origination = ch->in_room;
  
  if(ch->in_room != NOWHERE) {
    // Couldn't move char from the room
    if(char_from_room(ch) == 0) {
      log("Character was not in NOWHERE, but I couldn't move it!",
	   OVERSEER, LOG_BUG);
      return eINTERNAL_ERROR;
    }
  }
  
  // Couldn't move char to new room
  if(char_to_room(ch, dest) == 0) {
    // Now we have real problems
    if(char_to_room(ch, origination) == 0) {
      fprintf(stderr, "FATAL: Character stuck in NOWHERE: %s.\n",
	      GET_NAME(ch));
      abort();
    }
    logf(OVERSEER, LOG_BUG, "Could not move %s to destination: %d",
         GET_NAME(ch), world[dest].number);
    return eINTERNAL_ERROR;
  }
  
  // At this point, the character is happily in the new room
  return eSUCCESS;
}

int do_climb(struct char_data *ch, char *argument, int cmd)
{
   char buf[MAX_INPUT_LENGTH];
   obj_data * obj = NULL;

   one_argument(argument, buf);

   if(!*buf) {
      send_to_char("Climb what?\r\n", ch);
      return eSUCCESS;
   }

   if(!(obj = get_obj_in_list_vis(ch, buf, world[ch->in_room].contents))) {
      send_to_char("Climb what?\r\n", ch);
      return eSUCCESS;
   }

   if(obj->obj_flags.type_flag != ITEM_CLIMBABLE) {
      send_to_char("You can't climb that.\n\r", ch);
      return eSUCCESS;
   }

   int dest = obj->obj_flags.value[0];

   if(real_room(dest) < 0) {
      logf(IMMORTAL, LOG_WORLD, "Illegal desination in climb object %d.", obj_index[obj->item_number].virt);
      send_to_char("Climbable object broken.  Tell a god.\r\n", ch);
      return eFAILURE|eINTERNAL_ERROR;
   }

   int retval = move_char(ch, real_room(dest));
   
   sprintf(buf, "You climb %s.\r\n", obj->short_description);
   send_to_char(buf, ch);
   act("$n climbs $p.", ch, obj, 0, TO_ROOM, INVIS_NULL);
  
   if(SOMEONE_DIED(retval))
      return retval;

   do_look(ch, "", 9);
   return eSUCCESS;
}
