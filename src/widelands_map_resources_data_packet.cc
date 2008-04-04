/*
 * Copyright (C) 2002-2004, 2006-2008 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "widelands_map_resources_data_packet.h"
#include "editor_game_base.h"
#include "map.h"
#include "world.h"
#include "widelands_fileread.h"
#include "widelands_filewrite.h"
#include "widelands_map_data_packet_ids.h"

#include "log.h"

namespace Widelands {

#define CURRENT_PACKET_VERSION 1


void Map_Resources_Data_Packet::Read
	(FileSystem            & fs,
	 Editor_Game_Base      * egbase,
	 bool,
	 Map_Map_Object_Loader * const)
throw (_wexception)
{
	FileRead fr;

	fr.Open(fs, "binary/resource");

	Map* map=egbase->get_map();
	World* world=egbase->get_map()->get_world();

	const uint16_t packet_version = fr.Unsigned16();
	if (packet_version == CURRENT_PACKET_VERSION) {
		int32_t nr_res=fr.Unsigned16();
		if (nr_res > world->get_nr_resources())
			log
				("WARNING: Number of resources in map (%i) is bigger than in world "
				 "(%i)",
				 nr_res, world->get_nr_resources());

		// construct ids and map
		std::map<uint8_t, int32_t> smap;
		for (int32_t i = 0; i < nr_res; ++i) {
			int32_t id=fr.Unsigned16();
			char const * const buffer = fr.CString();
			int32_t res=world->get_resource(buffer);
			if (res==-1) throw wexception("Resource '%s' exists in map, not in world!", buffer);
			smap[id]=res;
		}

		for (uint16_t y = 0; y < map->get_height(); ++y) {
			for (uint16_t x = 0; x < map->get_width(); ++x) {
				int32_t id=fr.Unsigned8();
				int32_t found_amount=fr.Unsigned8();
				int32_t start_amount=0;
				int32_t amount=0;
				amount=found_amount;
				start_amount=fr.Unsigned8();

				int32_t set_id, set_amount, set_start_amount;
				//  if amount is zero, theres nothing here
				if (!amount) {
					set_id=0;
					set_amount=0;
					set_start_amount=0;
				} else {
					set_id=smap[id];
					set_amount=amount;
					set_start_amount=start_amount;
				}

				if (set_id == -1)
					throw "Unknown resource in map file. It is not in world!\n";
				egbase->map()[Coords(x, y)].set_resources(set_id, set_amount);
				egbase->map()[Coords(x, y)].set_starting_res_amount(set_start_amount);
			}
		}
	} else
		throw wexception
			("Unknown version in Map_Resources_Data_Packet: %u", packet_version);
}


/*
 * Ok, when we're called from the editor, the default resources
 * are not set, which is ok.
 * When we are called from a game, the default resources are set
 * which is also ok. But this is one reason why save game != saved map
 * in nearly all cases.
 */
void Map_Resources_Data_Packet::Write
	(FileSystem & fs, Editor_Game_Base * egbase, Map_Map_Object_Saver * const)
throw (_wexception)
{
	FileWrite fw;

	fw.Unsigned16(CURRENT_PACKET_VERSION);

	// This is a bit more complicated saved so that the order of loading
	// of the resources at run time doesn't matter.
	// (saved like terrains)
	// Write the number of resources
	World* world=egbase->get_map()->get_world();
	int32_t nr_res=world->get_nr_resources();
	fw.Unsigned16(nr_res);

	//  write all resources names and their id's
	std::map<std::string, uint8_t> smap;
	for (int32_t i = 0; i < nr_res; ++i) {
		Resource_Descr* res=world->get_resource(i);
		smap[res->name().c_str()]=i;
		fw.Unsigned16(i);
		fw.CString(res->name().c_str());
	}

	//  Now, all resouces as uint8_ts in order
	//  - resource id
	//  - amount
	Map const & map = egbase->map();
	for (uint16_t y = 0; y < map.get_height(); ++y) {
		for (uint16_t x = 0; x < map.get_width(); ++x) {
			Field const & f = map[Coords(x, y)];
			int32_t       res          = f.get_resources          ();
			int32_t const       amount = f.get_resources_amount   ();
			int32_t const start_amount = f.get_starting_res_amount();
			if (!amount)
				res = 0;
			fw.Unsigned8(res);
			fw.Unsigned8(amount);
			fw.Unsigned8(start_amount);
		}
	}

	fw.Write(fs, "binary/resource");
}

};
