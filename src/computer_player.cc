/*
 * Copyright (C) 2004, 2006 by the Widelands Development Team
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

#include <queue>
#include <typeinfo>
#include <algorithm>
#include "error.h"
#include "map.h"
#include "world.h"
#include "transport.h"
#include "player.h"
#include "tribe.h"
#include "constructionsite.h"
#include "productionsite.h"
#include "militarysite.h"
#include "computer_player.h"
#include "computer_player_hints.h"

#define FIELD_UPDATE_INTERVAL	1000

class CheckStepRoadAI : public CheckStep {
public:
	CheckStepRoadAI(Player* pl, uchar mc, bool oe)
		: player(pl), movecaps(mc), openend(oe)
	{ }

	void set_openend (bool oe)
	{ openend=oe; }

	virtual bool allowed(Map* map, FCoords start, FCoords end, int dir, StepId id) const;
	virtual bool reachabledest(Map* map, FCoords dest) const;

//private:
	Player*		player;
	uchar		movecaps;
	bool		openend;
};

Computer_Player::Computer_Player (Game *g, uchar pid)
{
	game = g;
	player_number = pid;

	map=0;
	world=0;
}

// when Computer_Player is constructed, some information is not yet available (e.g. world)
void Computer_Player::late_initialization ()
{
	map = game->get_map();
	world = map->get_world();
	assert (world!=0);

	player = game->get_player(player_number);
	tribe = player->get_tribe();

	log ("ComputerPlayer(%d): initializing\n", player_number);

	wares=new WareObserver[tribe->get_nrwares()];
	for (int i=0; i<tribe->get_nrwares(); i++) {
	    wares[i].producers=0;
	    wares[i].consumers=0;
	    wares[i].preciousness=0;
	}
/*=====================*/
// Tribe specific stuff
// ToDo: This should be defined in tribes "conf-file"
	const char* quarry="quarry";
	const char* lumberjack="lumberjack";
	const char* ranger="ranger";

if(tribe->m_name=="barbarians"){
	wares[tribe->get_safe_ware_index("trunk")].preciousness=4;
	wares[tribe->get_safe_ware_index("raw_stone")].preciousness=3;
	wares[tribe->get_safe_ware_index("grindstone")].preciousness=2;
	wares[tribe->get_safe_ware_index("blackwood")].preciousness=1;
}
else{
if(tribe->m_name=="empire"){
	wares[tribe->get_safe_ware_index("trunk")].preciousness=4;
	wares[tribe->get_safe_ware_index("stone")].preciousness=3;
	wares[tribe->get_safe_ware_index("wood")].preciousness=2;
	wares[tribe->get_safe_ware_index("marble")].preciousness=1;
ranger="forester";
	}
}
/*=====================*/

	// collect information about which buildings our tribe can construct
	for (int i=0; i<tribe->get_nrbuildings();i++) {
		Building_Descr* bld=tribe->get_building_descr(i);
		log ("ComputerPlayer(%d): I can build '%s', id is %d\n",player_number,bld->get_name(),i);

		buildings.push_back (BuildingObserver());

		BuildingObserver& bo=buildings.back();
		bo.name=bld->get_name();
		bo.id=i;
		bo.desc=bld;
		bo.hints=bld->get_hints();
		bo.type=BuildingObserver::BORING;
		bo.cnt_built=0;
		bo.cnt_under_construction=0;
		bo.production_hint=-1;

		bo.is_buildable=bld->get_buildable();

		bo.need_trees=false;
		bo.need_stones=false;

		// FIXME: define these properties in the building's conf file
		if (!strcmp(bld->get_name(), quarry))
		    bo.need_stones=true;

		if (!strcmp(bld->get_name(), lumberjack))
		    bo.need_trees=true;

		if (!strcmp(bld->get_name(), ranger))
		    bo.production_hint=tribe->get_safe_ware_index("trunk");

		if (typeid(*bld)==typeid(ConstructionSite_Descr)) {
			bo.type=BuildingObserver::CONSTRUCTIONSITE;
			continue;
		}

		if (typeid(*bld)==typeid(MilitarySite_Descr)) {
			bo.type=BuildingObserver::MILITARYSITE;
			continue;
		}

		if (typeid(*bld)==typeid(ProductionSite_Descr)) {
		    ProductionSite_Descr* prod=static_cast<ProductionSite_Descr*>(bld);

		    bo.type=bld->get_ismine()
				?BuildingObserver::MINE
				:BuildingObserver::PRODUCTIONSITE;

		    for (std::vector<Input>::const_iterator j=prod->get_inputs()->begin();j!=prod->get_inputs()->end();j++)
			bo.inputs.push_back (tribe->get_safe_ware_index(j->get_ware()->get_name()));

		    for (std::set<std::string>::const_iterator j=prod->get_outputs()->begin();j!=prod->get_outputs()->end();j++)
			bo.outputs.push_back (tribe->get_safe_ware_index(j->c_str()));

		    continue;
		}
	}

	total_constructionsites=0;
	next_construction_due=0;
	next_road_due=0;
	next_productionsite_check_due=0;
	inhibit_road_building=0;
}

Computer_Player::~Computer_Player ()
{
}

Computer_Player::BuildingObserver& Computer_Player::get_building_observer (const char* name)
{
	if (map==0)
		late_initialization ();

	for (std::list<BuildingObserver>::iterator i=buildings.begin();i!=buildings.end();i++)
		if (!strcmp(i->name, name))
			return *i;

	throw wexception("Help: I don't know what to do with a %s", name);
}

void Computer_Player::think ()
{
	if (map==0)
		late_initialization ();

	// update statistics about buildable fields
	while (!buildable_fields.empty() && buildable_fields.front()->next_update_due<=game->get_gametime()) {
		BuildableField* bf=buildable_fields.front();

		// check whether we lost ownership of the field
		if (bf->coords.field->get_owned_by()!=player_number) {
			log ("ComputerPlayer(%d): lost field (%d,%d)\n", player_number, bf->coords.x, bf->coords.y);

			buildable_fields.pop_front();
			continue;
		}

		// check whether we can still construct regular buildings on the field
		if ((player->get_buildcaps(bf->coords) & BUILDCAPS_SIZEMASK)==0) {
			log ("ComputerPlayer(%d): field (%d,%d) can no longer be built upon\n", player_number, bf->coords.x, bf->coords.y);

			unusable_fields.push_back (bf->coords);
			delete bf;

			buildable_fields.pop_front();
			continue;
		}

		update_buildable_field (bf);
		bf->next_update_due=game->get_gametime() + FIELD_UPDATE_INTERVAL;

		buildable_fields.push_back (bf);
		buildable_fields.pop_front ();
	}

	// do the same for mineable fields
	while (!mineable_fields.empty() && mineable_fields.front()->next_update_due<=game->get_gametime()) {
		MineableField* mf=mineable_fields.front();

		// check whether we lost ownership of the field
		if (mf->coords.field->get_owned_by()!=player_number) {
			log ("ComputerPlayer(%d): lost field (%d,%d)\n", player_number, mf->coords.x, mf->coords.y);

			mineable_fields.pop_front();
			continue;
		}

		// check whether we can still construct regular buildings on the field
		if ((player->get_buildcaps(mf->coords) & BUILDCAPS_MINE)==0) {
			log ("ComputerPlayer(%d): field (%d,%d) can no longer be mined upon\n", player_number, mf->coords.x, mf->coords.y);

			unusable_fields.push_back (mf->coords);
			delete mf;

			mineable_fields.pop_front();
			continue;
		}

		update_mineable_field (mf);
		mf->next_update_due=game->get_gametime() + FIELD_UPDATE_INTERVAL;

		mineable_fields.push_back (mf);
		mineable_fields.pop_front ();
	}

	for (std::list<FCoords>::iterator i=unusable_fields.begin(); i!=unusable_fields.end();) {
		// check whether we lost ownership of the field
		if (i->field->get_owned_by()!=player_number) {
			log ("ComputerPlayer(%d): lost field (%d,%d)\n", player_number, i->x, i->y);
			i=unusable_fields.erase(i);
			continue;
		}

		// check whether building capabilities have improved
		if ((player->get_buildcaps(*i) & BUILDCAPS_SIZEMASK) != 0) {
			log ("ComputerPlayer(%d): field (%d,%d) can now be built upon\n", player_number, i->x, i->y);
			buildable_fields.push_back (new BuildableField(*i));
			i=unusable_fields.erase(i);

			update_buildable_field (buildable_fields.back());
			continue;
		}

		if ((player->get_buildcaps(*i) & BUILDCAPS_MINE) != 0) {
			log ("ComputerPlayer(%d): field (%d,%d) can now be mined upon\n", player_number, i->x, i->y);
			mineable_fields.push_back (new MineableField(*i));
			i=unusable_fields.erase(i);

			update_mineable_field (mineable_fields.back());
			continue;
		}

		i++;
	}

	// wait a moment so that all fields are classified
	if (next_construction_due==0)
	    next_construction_due=game->get_gametime() + 1000;

	// now build something if possible
	if (next_construction_due<=game->get_gametime()) {
	    next_construction_due=game->get_gametime() + 2000;

	    if (construct_building()) {
		inhibit_road_building=game->get_gametime() + 2500;
		return;
	    }
	}

	// verify that our production sites are doing well
	if (next_productionsite_check_due<=game->get_gametime() && !productionsites.empty()) {
	    next_productionsite_check_due=game->get_gametime() + 2000;

	    check_productionsite (productionsites.front());

	    productionsites.push_back (productionsites.front());
	    productionsites.pop_front ();
	}

	// if nothing else is to do, update flags and economies
	while (!new_flags.empty()) {
		Flag* flag=new_flags.front();
		new_flags.pop_front();

		get_economy_observer(flag->get_economy())->flags.push_back (flag);
	}

	for (std::list<EconomyObserver*>::iterator i=economies.begin(); i!=economies.end();) {
		// check if any flag has changed its economy
		for (std::list<Flag*>::iterator j=(*i)->flags.begin(); j!=(*i)->flags.end();) {
			if ((*i)->economy!=(*j)->get_economy()) {
				log ("ComputerPlayer(%d): flag at (%d,%d) changed economy\n", player_number, (*j)->get_position().x, (*j)->get_position().y);

				get_economy_observer((*j)->get_economy())->flags.push_back (*j);
				j=(*i)->flags.erase(j);
				continue;
			}

			j++;
		}

		// if there are no more flags in this economy, we no longer need its observer
		if ((*i)->flags.empty()) {
			delete *i;
			i=economies.erase(i);
			continue;
		}

		i++;
	}

	if (next_road_due<=game->get_gametime() && inhibit_road_building<=game->get_gametime()) {
	    next_road_due=game->get_gametime() + 1000;

	    construct_roads ();
	}

/*	if (!economies.empty() && inhibit_road_building<=game->get_gametime()) {
		EconomyObserver* eco=economies.front();

		bool finish=false;

		// try to connect to another economy
    		if (economies.size()>1)
		    finish=connect_flag_to_another_economy(eco->flags.front());

		if (!finish)
		    finish=improve_roads(eco->flags.front());

		// cycle through flags one at a time
		eco->flags.push_back (eco->flags.front());
		eco->flags.pop_front ();

		// and cycle through economies
		economies.push_back (eco);
		economies.pop_front();

		if (finish)
		    return;
	}*/

	// force a split on roads that are extremely long
	// note that having too many flags causes a loss of building capabilities
	if (!roads.empty()) {
		const Path& path=roads.front()->get_path();

		if (path.get_nsteps()>6) {
			CoordPath cp(path);
			int i;

			// try to split near the middle
			for (i=0;i<cp.get_nsteps()/2-2;i++) {
				Field* f;

				f=map->get_field(cp.get_coords()[cp.get_nsteps()/2-i]);
				if ((f->get_caps()&BUILDCAPS_FLAG)!=0) {
					game->send_player_build_flag (player_number, cp.get_coords()[cp.get_nsteps()/2-i]);
					return;
				}

				f=map->get_field(cp.get_coords()[cp.get_nsteps()/2+i+1]);
				if ((f->get_caps()&BUILDCAPS_FLAG)!=0) {
					game->send_player_build_flag (player_number, cp.get_coords()[cp.get_nsteps()/2+i+1]);
					return;
				}
			}
		}

		roads.push_back (roads.front());
		roads.pop_front ();
	}
}

bool Computer_Player::construct_building ()
{
	int spots_avail[4];

	for (int i=0;i<4;i++)
		spots_avail[i]=0;

	for (std::list<BuildableField*>::iterator i=buildable_fields.begin(); i!=buildable_fields.end(); i++)
		spots_avail[(*i)->coords.field->get_caps() & BUILDCAPS_SIZEMASK]++;

	int expand_factor=1;

	if (spots_avail[BUILDCAPS_BIG]<2)
		expand_factor++;
	if (spots_avail[BUILDCAPS_MEDIUM]+spots_avail[BUILDCAPS_BIG]<4)
		expand_factor++;
	if (spots_avail[BUILDCAPS_SMALL]+spots_avail[BUILDCAPS_MEDIUM]+spots_avail[BUILDCAPS_BIG]<8)
		expand_factor++;

	int proposed_building=-1;
	int proposed_priority=0;
	Coords proposed_coords;

	// first scan all buildable fields for regular buildings
	for (std::list<BuildableField*>::iterator i=buildable_fields.begin(); i!=buildable_fields.end(); i++) {
		BuildableField* bf=*i;

		if (!bf->reachable)
			continue;

		int maxsize=bf->coords.field->get_caps() & BUILDCAPS_SIZEMASK;
		int prio;

		std::list<BuildingObserver>::iterator j;
		for (j=buildings.begin();j!=buildings.end();j++) {
		    if (!j->is_buildable)
			    continue;

		    if (j->type==BuildingObserver::MINE)
			    continue;

		    if (j->desc->get_size()>maxsize)
			    continue;

		    prio=0;

		    if (j->type==BuildingObserver::MILITARYSITE) {
			    prio=(bf->unowned_land_nearby - bf->military_influence*2) * expand_factor / 4;

			    if (bf->avoid_military)
				prio=prio/3 - 6;

			    prio-=spots_avail[BUILDCAPS_BIG]/2;
			    prio-=spots_avail[BUILDCAPS_MEDIUM]/4;
			    prio-=spots_avail[BUILDCAPS_SMALL]/8;
		    }

		    if (j->type==BuildingObserver::PRODUCTIONSITE) {
			    if (j->need_trees)
				    prio+=bf->trees_nearby - 6*bf->tree_consumers_nearby - 2;

			    if (j->need_stones)
				    prio+=bf->stones_nearby - 6*bf->stone_consumers_nearby - 2;

			    if ((j->need_trees || j->need_stones) && j->cnt_built==0 && j->cnt_under_construction==0)
				    prio*=2;

			    if (!j->need_trees && !j->need_stones) {
				if (j->cnt_built+j->cnt_under_construction==0)
				    prio+=2;

				for (unsigned int k=0; k<j->inputs.size(); k++) {
				    prio+=8*wares[j->inputs[k]].producers;
				    prio-=4*wares[j->inputs[k]].consumers;
				}

				for (unsigned int k=0; k<j->outputs.size(); k++) {
				    prio-=12*wares[j->outputs[k]].producers;
				    prio+=8*wares[j->outputs[k]].consumers;
				    prio+=4*wares[j->outputs[k]].preciousness;

				    if (j->cnt_built+j->cnt_under_construction==0 &&
					wares[j->outputs[k]].consumers>0)
					prio+=8; // add a big bonus
				}

				if (j->production_hint>=0) {
				    prio-=6*(j->cnt_built+j->cnt_under_construction);
				    prio+=4*wares[j->production_hint].consumers;
				    prio+=2*wares[j->production_hint].preciousness;
				}
			    }

			    prio-=2*j->cnt_under_construction*(j->cnt_under_construction+1);
		    }

		    if (bf->preferred)
			prio+=prio/2 + 1;
		    else
			prio--;

		    // don't waste good land for small huts
		    prio-=(maxsize - j->desc->get_size()) * 3;

		    if (prio>proposed_priority) {
			    proposed_building=j->id;
			    proposed_priority=prio;
			    proposed_coords=bf->coords;
		    }
		}
	}

	// then try all mines
	for (std::list<BuildingObserver>::iterator i=buildings.begin();i!=buildings.end();i++) {
		if (!i->is_buildable || i->type!=BuildingObserver::MINE)
			continue;

		for (std::list<MineableField*>::iterator j=mineable_fields.begin(); j!=mineable_fields.end(); j++) {
			MineableField* mf=*j;
			int prio=-1;

			if (i->hints->get_need_map_resource()!=0) {
				int res=world->get_resource(i->hints->get_need_map_resource());

				if (mf->coords.field->get_resources()!=res)
					continue;

				prio+=mf->coords.field->get_resources_amount();
			}

			WareObserver& output=wares[i->outputs[0]];
			if (output.consumers>0)
				prio*=2;

			prio-=2 * mf->mines_nearby * mf->mines_nearby;
			prio-=i->cnt_built*3;
			prio-=i->cnt_under_construction*8;

			if (prio>proposed_priority) {
				proposed_building=i->id;
				proposed_priority=prio;
				proposed_coords=mf->coords;
			}
		}
	}

	if (proposed_building<0)
		return false;

        // don't have too many construction sites
        if (proposed_priority<total_constructionsites*total_constructionsites)
		return false;

	// if we want to construct a new building, send the command now
	log ("ComputerPlayer(%d): want to construct building %d\n", player_number, proposed_building);
	game->send_player_build (player_number, proposed_coords, proposed_building);

	return true;
}

void Computer_Player::check_productionsite (ProductionSiteObserver& site)
{
	log ("ComputerPlayer(%d): checking %s\n", player_number, site.bo->desc->get_name());

	if (site.bo->need_trees &&
	    map->find_immovables(site.site->get_position(), 8, 0,
	    FindImmovableAttribute(Map_Object_Descr::get_attribute_id("tree")))==0) {

	    log ("ComputerPlayer(%d): out of resources, destructing\n", player_number);
	    game->send_player_bulldoze (site.site);
	    return;
	}

	if (site.bo->need_stones &&
	    map->find_immovables(site.site->get_position(), 8, 0,
	    FindImmovableAttribute(Map_Object_Descr::get_attribute_id("stone")))==0) {

	    log ("ComputerPlayer(%d): out of resources, destructing\n", player_number);
	    game->send_player_bulldoze (site.site);
	    return;
	}
}

struct FindFieldUnowned:FindField {
	virtual bool accept (const FCoords) const;
};

bool FindFieldUnowned::accept (const FCoords fc) const
{
	// when looking for unowned terrain to acquire, we are actually
	// only interested in fields we can walk on
	return fc.field->get_owned_by()==0 && (fc.field->get_caps()&MOVECAPS_WALK);
}

void Computer_Player::update_buildable_field (BuildableField* field)
{
	// look if there is any unowned land nearby
	FindFieldUnowned find_unowned;

	field->unowned_land_nearby=map->find_fields(field->coords, 8, 0, find_unowned);

	// collect information about resources in the area
	std::vector<ImmovableFound> immovables;

	const int tree_attr=Map_Object_Descr::get_attribute_id("tree");
	const int stone_attr=Map_Object_Descr::get_attribute_id("stone");

	map->find_immovables (field->coords, 8, &immovables);

	field->reachable=false;
	field->preferred=false;
	field->avoid_military=false;

	field->military_influence=0;
	field->trees_nearby=0;
	field->stones_nearby=0;
	field->tree_consumers_nearby=0;
	field->stone_consumers_nearby=0;

	FCoords fse;
	map->get_neighbour (field->coords, Map_Object::WALK_SE, &fse);

	BaseImmovable* imm=fse.field->get_immovable();
	if (imm!=0) {
	    if (imm->get_type()==BaseImmovable::FLAG)
		field->preferred=true;

	    if (imm->get_type()==BaseImmovable::ROAD && (fse.field->get_caps() & BUILDCAPS_FLAG))
		field->preferred=true;
	}

	for (unsigned int i=0;i<immovables.size();i++) {
		if (immovables[i].object->get_type()==BaseImmovable::FLAG)
			field->reachable=true;

		if (immovables[i].object->get_type()==BaseImmovable::BUILDING) {
			Building* bld=static_cast<Building*>(immovables[i].object);

			if (bld->get_building_type()==Building::CONSTRUCTIONSITE) {
			    Building_Descr* con=static_cast<ConstructionSite*>(bld)->get_building();

			    if (typeid(*con)==typeid(MilitarySite_Descr)) {
				MilitarySite_Descr* mil=static_cast<MilitarySite_Descr*>(con);

				int v=mil->get_conquers() - map->calc_distance(field->coords, immovables[i].coords);

				if (v>0) {
				    field->military_influence+=v*(v+2)*6;
				    field->avoid_military=true;
				}
			    }

			    if (typeid(*con)==typeid(ProductionSite_Descr))
				consider_productionsite_influence (field, immovables[i].coords,
					get_building_observer(con->get_name()));
			}

			if (bld->get_building_type()==Building::MILITARYSITE) {
			    MilitarySite* mil=static_cast<MilitarySite*>(bld);

			    int v=mil->get_conquers() - map->calc_distance(field->coords, immovables[i].coords);

			    if (v>0)
				field->military_influence+=v*v*mil->get_capacity();
			}

			if (bld->get_building_type()==Building::PRODUCTIONSITE)
			    consider_productionsite_influence (field, immovables[i].coords,
				    get_building_observer(bld->get_name()));

			continue;
		}

		if (immovables[i].object->has_attribute(tree_attr))
			field->trees_nearby++;

		if (immovables[i].object->has_attribute(stone_attr))
			field->stones_nearby++;
	}
}

void Computer_Player::update_mineable_field (MineableField* field)
{
	// collect information about resources in the area
	std::vector<ImmovableFound> immovables;

	map->find_immovables (field->coords, 6, &immovables);

	field->reachable=false;
	field->preferred=false;
	field->mines_nearby=true;

	FCoords fse;
	map->get_neighbour (field->coords, Map_Object::WALK_SE, &fse);

	BaseImmovable* imm=fse.field->get_immovable();
	if (imm!=0) {
	    if (imm->get_type()==BaseImmovable::FLAG)
		field->preferred=true;

	    if (imm->get_type()==BaseImmovable::ROAD && (fse.field->get_caps() & BUILDCAPS_FLAG))
		field->preferred=true;
	}

	for (unsigned int i=0;i<immovables.size();i++) {
		if (immovables[i].object->get_type()==BaseImmovable::FLAG)
			field->reachable=true;

		if (immovables[i].object->get_type()==BaseImmovable::BUILDING &&
		    (player->get_buildcaps(immovables[i].coords)&BUILDCAPS_MINE)!=0) {
			Building* bld=static_cast<Building*>(immovables[i].object);

			if (bld->get_building_type()==Building::CONSTRUCTIONSITE ||
			    bld->get_building_type()==Building::PRODUCTIONSITE)
				field->mines_nearby++;
		}
	}
}

void Computer_Player::consider_productionsite_influence
(BuildableField * field, const Coords &, const BuildingObserver & bo)
{
	if (bo.need_trees)
		field->tree_consumers_nearby++;

	if (bo.need_stones)
		field->stone_consumers_nearby++;
}

Computer_Player::EconomyObserver* Computer_Player::get_economy_observer (Economy* economy)
{
	std::list<EconomyObserver*>::iterator i;

	for (i=economies.begin(); i!=economies.end(); i++)
		if ((*i)->economy==economy)
			return *i;

	economies.push_front (new EconomyObserver(economy));

	return economies.front();
}

void Computer_Player::gain_building (Building* b)
{
	BuildingObserver& bo=get_building_observer(b->get_name());

	if (bo.type==BuildingObserver::CONSTRUCTIONSITE) {
		get_building_observer(static_cast<ConstructionSite*>(b)->get_building()->get_name()).cnt_under_construction++;
	    	total_constructionsites++;
	}
	else {
		bo.cnt_built++;

		if (bo.type==BuildingObserver::PRODUCTIONSITE) {
			productionsites.push_back (ProductionSiteObserver());
			productionsites.back().site=static_cast<ProductionSite*>(b);
			productionsites.back().bo=&bo;

			for (unsigned int i=0;i<bo.outputs.size();i++)
		    		wares[bo.outputs[i]].producers++;

			for (unsigned int i=0;i<bo.inputs.size();i++)
				wares[bo.inputs[i]].consumers++;
		}
	}
}

void Computer_Player::lose_building (Building* b)
{
	BuildingObserver& bo=get_building_observer(b->get_name());

	if (bo.type==BuildingObserver::CONSTRUCTIONSITE) {
		get_building_observer(static_cast<ConstructionSite*>(b)->get_building()->get_name()).cnt_under_construction--;
		total_constructionsites--;
	}
	else {
		bo.cnt_built--;

		if (bo.type==BuildingObserver::PRODUCTIONSITE) {
			for (std::list<ProductionSiteObserver>::iterator i=productionsites.begin(); i!=productionsites.end(); i++)
				if (i->site==b) {
					productionsites.erase (i);
					break;
				}

			for (unsigned int i=0;i<bo.outputs.size();i++)
				wares[bo.outputs[i]].producers--;

			for (unsigned int i=0;i<bo.inputs.size();i++)
				wares[bo.inputs[i]].consumers--;
		}
	}
}

// Road building
struct FindFieldWithFlagOrRoad:FindField {
	Economy* economy;
	virtual bool accept(FCoords coord) const;
};

bool FindFieldWithFlagOrRoad::accept (FCoords fc) const
{
	BaseImmovable* imm=fc.field->get_immovable();

	if (imm==0)
		return false;

	if (imm->get_type()>=BaseImmovable::BUILDING && static_cast<PlayerImmovable*>(imm)->get_economy()==economy)
		return false;

	if (imm->get_type()==BaseImmovable::FLAG)
		return true;

	if (imm->get_type()==BaseImmovable::ROAD && (fc.field->get_caps()&BUILDCAPS_FLAG)!=0)
		return true;

	return false;
}

bool Computer_Player::connect_flag_to_another_economy (Flag* flag)
{
	FindFieldWithFlagOrRoad functor;
	CheckStepRoadAI check(player, MOVECAPS_WALK, true);
	std::vector<Coords> reachable;

	// first look for possible destinations
	functor.economy=flag->get_economy();
	map->find_reachable_fields
		(flag->get_position(), 16, &reachable, check, functor);

	if (reachable.empty())
		return false;

	// then choose the one closest to the originating flag
	int closest, distance;

	closest=0;
	distance=map->calc_distance(flag->get_position(), reachable[0]);
	for (unsigned int i=1; i<reachable.size(); i++) {
		int d=map->calc_distance(flag->get_position(), reachable[i]);

		if (d<distance) {
		    closest=i;
		    distance=d;
		}
	}

	// if we join a road and there is no flag yet, build one
	Field* field=map->get_field(reachable[closest]);
	if (field->get_immovable()->get_type()==BaseImmovable::ROAD)
		game->send_player_build_flag (player_number, reachable[closest]);

	// and finally build the road
	Path* path=new Path();
	check.set_openend (false);
	if
		(map->findpath(flag->get_position(), reachable[closest], 0, *path, check)
		 <
		 0)
		return false;

	game->send_player_build_road (player_number, path);
	return true;
}

struct NearFlag {
    Flag*	flag;
    long	cost;
    long	distance;

    NearFlag (Flag* f, long c, long d)
    { flag=f; cost=c; distance=d; }

    bool operator< (const NearFlag& f) const
    { return cost>f.cost; }

    bool operator== (const Flag* f) const
    { return flag==f; }
};

struct CompareDistance {
    bool operator() (const NearFlag& a, const NearFlag& b) const
    { return a.distance < b.distance; }
};

bool Computer_Player::improve_roads (Flag* flag)
{
	std::priority_queue<NearFlag> queue;
	std::vector<NearFlag> nearflags;
	unsigned int i;

	queue.push (NearFlag(flag, 0, 0));

	while (!queue.empty()) {
    	    std::vector<NearFlag>::iterator f=find(nearflags.begin(), nearflags.end(), queue.top().flag);
	    if (f!=nearflags.end()) {
		queue.pop ();
		continue;
	    }

	    nearflags.push_back (queue.top());
	    queue.pop ();

	    NearFlag& nf=nearflags.back();

	    for (i=1;i<=6;i++) {
		Road* road=nf.flag->get_road(i);

		if (!road) continue;

		Flag* endflag=road->get_flag(Road::FlagStart);
		if (endflag==nf.flag)
		    endflag=road->get_flag(Road::FlagEnd);

		long dist=map->calc_distance(flag->get_position(), endflag->get_position());
		if (dist>16)	// out of range
		    continue;

		queue.push (NearFlag(endflag, nf.cost+road->get_path().get_nsteps(), dist));
	    }
	}

	std::sort (nearflags.begin(), nearflags.end(), CompareDistance());

	CheckStepRoadAI check(player, MOVECAPS_WALK, false);

	for (i=1;i<nearflags.size();i++) {
	    NearFlag& nf=nearflags[i];

	    if (2*nf.distance+2>=nf.cost)
		continue;

	    Path* path=new Path();
		if
			(map->findpath
			 (flag->get_position(), nf.flag->get_position(), 0, *path, check)
			 >=
			 0
			 and
			 2 * path->get_nsteps() + 2 < nf.cost)
		{
			game->send_player_build_road (player_number, path);
			return true;
		}

	    delete path;
	}

	return false;
}

// this is called whenever we gain ownership of a PlayerImmovable
void Computer_Player::gain_immovable (PlayerImmovable* pi)
{
	switch (pi->get_type()) {
	    case BaseImmovable::BUILDING:
		gain_building (static_cast<Building*>(pi));
		break;
	    case BaseImmovable::FLAG:
		new_flags.push_back (static_cast<Flag*>(pi));
		break;
	    case BaseImmovable::ROAD:
		roads.push_front (static_cast<Road*>(pi));
		break;
	}
}

// this is called whenever we lose ownership of a PlayerImmovable
void Computer_Player::lose_immovable (PlayerImmovable* pi)
{
	switch (pi->get_type()) {
	    case BaseImmovable::BUILDING:
		lose_building (static_cast<Building*>(pi));
		break;
	    case BaseImmovable::FLAG:
		for (std::list<EconomyObserver*>::iterator i=economies.begin(); i!=economies.end(); i++)
		    for (std::list<Flag*>::iterator j=(*i)->flags.begin(); j!=(*i)->flags.end(); j++)
			if (*j==pi) {
			    (*i)->flags.erase (j);
			    break;
			}

		break;
	    case BaseImmovable::ROAD:
		roads.remove (static_cast<Road*>(pi));
		break;
	}
}

// this is called whenever we gain ownership of a field on the map
void Computer_Player::gain_field (const FCoords& fc)
{
	unusable_fields.push_back (fc);
}

// we don't use this - instead we check or fields regularly, see think()
void Computer_Player::lose_field (const FCoords &) {}


/* CheckStepRoadAI */
bool CheckStepRoadAI::allowed
(Map * map, FCoords, FCoords end, int, StepId id) const
{
	uchar endcaps = player->get_buildcaps(end);

	// Calculate cost and passability
	if (!(endcaps & movecaps)) {
		return false;
//		uchar startcaps = player->get_buildcaps(start);

//		if (!((endcaps & MOVECAPS_WALK) && (startcaps & movecaps & MOVECAPS_SWIM)))
//			return false;
	}

	// Check for blocking immovables
	BaseImmovable *imm = map->get_immovable(end);
	if (imm && imm->get_size() >= BaseImmovable::SMALL) {
		if (id!=stepLast && !openend)
			return false;

		if (imm->get_type()==Map_Object::FLAG)
			return true;

		if ((imm->get_type() != Map_Object::ROAD || !(endcaps & BUILDCAPS_FLAG)))
			return false;
	}

	return true;
}

bool CheckStepRoadAI::reachabledest(Map* map, FCoords dest) const
{
	uchar caps = dest.field->get_caps();

	if (!(caps & movecaps)) {
		if (!((movecaps & MOVECAPS_SWIM) && (caps & MOVECAPS_WALK)))
			return false;

		if (!map->can_reach_by_water(dest))
			return false;
	}

	return true;
}

struct WalkableSpot {
	Coords	coords;
	bool	hasflag;

	int	cost;
	void*	eco;

	short	from;
	short	neighbours[6];
};

void Computer_Player::construct_roads ()
{
	std::vector<WalkableSpot> spots;
	std::queue<int> queue;

	for (std::list<EconomyObserver*>::iterator i=economies.begin(); i!=economies.end(); i++)
	    for (std::list<Flag*>::iterator j=(*i)->flags.begin(); j!=(*i)->flags.end(); j++) {
		queue.push (spots.size());

		spots.push_back(WalkableSpot());
		spots.back().coords=(*j)->get_position();
		spots.back().hasflag=true;
		spots.back().cost=0;
		spots.back().eco=(*i)->economy;
		spots.back().from=-1;
	    }

	for (std::list<BuildableField*>::iterator i=buildable_fields.begin(); i!=buildable_fields.end(); i++) {
		spots.push_back(WalkableSpot());
		spots.back().coords=(*i)->coords;
		spots.back().hasflag=false;
		spots.back().cost=-1;
		spots.back().eco=0;
		spots.back().from=-1;
	}

	for (std::list<FCoords>::iterator i=unusable_fields.begin(); i!=unusable_fields.end(); i++) {
		if ((player->get_buildcaps(*i)&MOVECAPS_WALK)==0)
		    continue;

		BaseImmovable *imm=map->get_immovable(*i);
		if (imm && imm->get_type()==Map_Object::ROAD) {
		    if ((player->get_buildcaps(*i)&BUILDCAPS_FLAG)==0)
			continue;

		    queue.push (spots.size());

		    spots.push_back(WalkableSpot());
		    spots.back().coords=*i;
		    spots.back().hasflag=false;
		    spots.back().cost=0;
		    spots.back().eco=((Road*) imm)->get_flag(Road::FlagStart)->get_economy();
		    spots.back().from=-1;

		    continue;
		}

		if (imm && imm->get_size()>=BaseImmovable::SMALL)
		    continue;

		spots.push_back(WalkableSpot());
		spots.back().coords=*i;
		spots.back().hasflag=false;
		spots.back().cost=-1;
		spots.back().eco=0;
		spots.back().from=-1;
	}

	int i,j,k;
	for (i=0;i<(int) spots.size();i++)
	    for (j=0;j<6;j++) {
		Coords nc;

		map->get_neighbour (spots[i].coords, j+1, &nc);

		for (k=0;k<(int) spots.size();k++)
		    if (spots[k].coords==nc)
			break;

		spots[i].neighbours[j]=(k<(int) spots.size()) ? k : -1;
	    }

	log ("Computer_Player(%d): %d spots for road building\n", player_number, spots.size());

	while (!queue.empty()) {
	    WalkableSpot &from=spots[queue.front()];
	    queue.pop();

	    for (i=0;i<6;i++)
		if (from.neighbours[i]>=0) {
		    WalkableSpot &to=spots[from.neighbours[i]];

		    if (to.cost<0) {
    			to.cost=from.cost+1;
			to.eco=from.eco;
			to.from=&from - &spots.front();

			queue.push (&to - &spots.front());
			continue;
		    }

		    if (from.eco!=to.eco && to.cost>0) {
			std::list<Coords> pc;
			bool hasflag;

			pc.push_back (to.coords);
			i=to.from;
			hasflag=to.hasflag;
			while (i>=0) {
			    pc.push_back (spots[i].coords);
			    hasflag=spots[i].hasflag;
			    i=spots[i].from;
			}

			if (!hasflag)
			    game->send_player_build_flag (player_number, pc.back());

			pc.push_front (from.coords);
			i=from.from;
			hasflag=from.hasflag;
			while (i>=0) {
			    pc.push_front (spots[i].coords);
			    hasflag=spots[i].hasflag;
			    i=spots[i].from;
			}

			if (!hasflag)
			    game->send_player_build_flag (player_number, pc.front());

			log ("Computer_Player(%d): New road has length %d\n", player_number, pc.size());
			for (std::list<Coords>::iterator c=pc.begin(); c!=pc.end(); c++)
			    log ("Computer_Player: (%d,%d)\n", c->x, c->y);

	    		Path* path=new Path(map, pc.front());
			pc.pop_front();

			for (std::list<Coords>::iterator c=pc.begin(); c!=pc.end(); c++) {
			    int n=map->is_neighbour(path->get_end(), *c);
			    assert (n>=1 && n<=6);

			    path->append (n);
			    assert (path->get_end()==*c);
			}

			game->send_player_build_road (player_number, path);
			return;
		    }
		}
	}
}
