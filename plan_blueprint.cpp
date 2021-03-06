#include "ai.h"
#include "plan.h"
#include "room.h"

#include <tuple>

#include "modules/Buildings.h"

#include "df/building_civzonest.h"
#include "df/world.h"

REQUIRE_GLOBAL(world);

const int16_t Plan::MinX = -48, Plan::MinY = -22, Plan::MinZ = -5;
const int16_t Plan::MaxX = 48, Plan::MaxY = 22, Plan::MaxZ = 1;

const static int16_t farm_w = 3;
const static int16_t farm_h = 3;
const static int32_t dpf = farm_w * farm_h * dwarves_per_farmtile_num / dwarves_per_farmtile_den;
const static int32_t nrfarms = (220 + dpf - 1) / dpf + extra_farms;

static furniture *new_furniture(const std::string & item, int16_t x, int16_t y)
{
    furniture *f = new furniture();
    f->item = item;
    f->x = x;
    f->y = y;
    return f;
}

static furniture *new_furniture_with_users(const std::string & item, int16_t x, int16_t y, bool ignore = false)
{
    furniture *f = new_furniture(item, x, y);
    f->has_users = true;
    f->ignore = ignore;
    return f;
}

static furniture *new_cage_trap(int16_t x, int16_t y)
{
    furniture *f = new_furniture("trap", x, y);
    f->subtype = "cage";
    f->ignore = true;
    return f;
}

static furniture *new_door(int16_t x, int16_t y, bool internal = false)
{
    furniture *f = new_furniture("door", x, y);
    f->internal = internal;
    return f;
}

static furniture *new_dig(df::tile_dig_designation d, int16_t x, int16_t y, int16_t z = 0)
{
    furniture *f = new furniture();
    f->dig = d;
    f->x = x;
    f->y = y;
    f->z = z;
    return f;
}

static furniture *new_hive_floor(int16_t x, int16_t y)
{
    furniture *f = new_furniture("hive", x, y);
    f->construction = construction_type::Floor;
    return f;
}

static furniture *new_construction(df::construction_type c, int16_t x, int16_t y, int16_t z = 0)
{
    furniture *f = new furniture();
    f->construction = c;
    f->x = x;
    f->y = y;
    f->z = z;
    return f;
}

static furniture *new_wall(int16_t x, int16_t y, int16_t z = 0)
{
    furniture *f = new_construction(construction_type::Wall, x, y, z);
    f->dig = tile_dig_designation::No;
    return f;
}

static furniture *new_well(int16_t x, int16_t y)
{
    furniture *f = new_furniture("well", x, y);
    f->dig = tile_dig_designation::Channel;
    return f;
}

static furniture *new_cistern_lever(int16_t x, int16_t y, const std::string & way)
{
    furniture *f = new_furniture("trap", x, y);
    f->subtype = "lever";
    f->way = way;
    return f;
}

static furniture *new_cistern_floodgate(int16_t x, int16_t y, const std::string & way, bool ignore = false)
{
    furniture *f = new_furniture("floodgate", x, y);
    f->way = way;
    f->ignore = ignore;
    return f;
}

command_result Plan::setup_ready(color_ostream & out)
{
    digroom(out, find_room(room_type::workshop, [](room *r) -> bool { return r->subtype == "Masons" && r->level == 0; }));
    digroom(out, find_room(room_type::workshop, [](room *r) -> bool { return r->subtype == "Carpenters" && r->level == 0; }));
    find_room(room_type::workshop, [](room *r) -> bool { return r->subtype.empty() && r->level == 0; })->subtype = "Masons";
    find_room(room_type::workshop, [](room *r) -> bool { return r->subtype.empty() && r->level == 1; })->subtype = "Masons";
    find_room(room_type::workshop, [](room *r) -> bool { return r->subtype.empty() && r->level == 2; })->subtype = "Masons";
    wantdig(out, find_room(room_type::stockpile, [](room *r) -> bool { return r->subtype == "food" && r->level == 0 && r->workshop && r->workshop->type == room_type::farmplot; }));

    dig_garbagedump(out);

    return CR_OK;
}

command_result Plan::setup_blueprint(color_ostream & out)
{
    command_result res;
    // TODO use existing fort facilities (so we can relay the user or continue from a save)
    ai->debug(out, "setting up fort blueprint...");
    // TODO place fort body first, have main stair stop before surface, and place trade depot on path to surface
    res = scan_fort_entrance(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "blueprint found entrance");
    // TODO if no room for fort body, make surface fort
    res = scan_fort_body(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "blueprint found body");
    res = setup_blueprint_rooms(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "blueprint found rooms");
    // ensure traps are on the surface
    for (auto i = fort_entrance->layout.begin(); i != fort_entrance->layout.end(); i++)
    {
        (*i)->z = surface_tile_at(fort_entrance->min.x + (*i)->x, fort_entrance->min.y + (*i)->y, true).z - fort_entrance->min.z;
    }
    fort_entrance->layout.erase(std::remove_if(fort_entrance->layout.begin(), fort_entrance->layout.end(), [this](furniture *i) -> bool
                {
                    df::coord t = fort_entrance->min + df::coord(i->x, i->y, i->z - 1);
                    df::tiletype *tt = Maps::getTileType(t);
                    if (!tt)
                    {
                        delete i;
                        return true;
                    }
                    df::tiletype_material tm = ENUM_ATTR(tiletype, material, *tt);
                    if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *tt)) != tiletype_shape_basic::Wall || (tm != tiletype_material::STONE && tm != tiletype_material::MINERAL && tm != tiletype_material::SOIL && tm != tiletype_material::ROOT && (!allow_ice || tm != tiletype_material::FROZEN_LIQUID)))
                    {
                        delete i;
                        return true;
                    }
                    return false;
                }), fort_entrance->layout.end());
    res = list_map_veins(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "blueprint found veins");
    res = setup_outdoor_gathering_zones(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "blueprint outdoor gathering zones");
    res = setup_blueprint_caverns(out);
    if (res == CR_OK)
        ai->debug(out, "blueprint found caverns");
    res = make_map_walkable(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "LET THE GAME BEGIN!");
    return CR_OK;
}

// search a valid tile for fortress entrance
command_result Plan::scan_fort_entrance(color_ostream & out)
{
    // map center
    int16_t cx = world->map.x_count / 2;
    int16_t cy = world->map.y_count / 2;
    df::coord center = surface_tile_at(cx, cy, true);

    df::coord ent0 = spiral_search(center, [this](df::coord t0) -> bool
            {
                // test the whole map for 3x5 clear spots
                df::coord t = surface_tile_at(t0.x, t0.y);
                if (!t.isValid())
                    return false;

                // make sure we're not too close to the edge of the map.
                if (t.x + MinX < 0 || t.x + MaxX >= world->map.x_count ||
                        t.y + MinY < 0 || t.y + MaxY >= world->map.y_count ||
                        t.z + MinZ < 0 || t.z + MaxZ >= world->map.z_count)
                {
                    return false;
                }

                for (int16_t _x = -1; _x <= 1; _x++)
                {
                    for (int16_t _y = -2; _y <= 2; _y++)
                    {
                        df::tiletype tt = *Maps::getTileType(t + df::coord(_x, _y, -1));
                        if (ENUM_ATTR(tiletype, shape, tt) != tiletype_shape::WALL)
                            return false;
                        df::tiletype_material tm = ENUM_ATTR(tiletype, material, tt);
                        if (!allow_ice &&
                                tm != tiletype_material::STONE &&
                                tm != tiletype_material::MINERAL &&
                                tm != tiletype_material::SOIL &&
                                tm != tiletype_material::ROOT)
                            return false;
                        df::coord ttt = t + df::coord(_x, _y, 0);
                        if (ENUM_ATTR(tiletype, shape, *Maps::getTileType(ttt)) != tiletype_shape::FLOOR)
                            return false;
                        df::tile_designation td = *Maps::getTileDesignation(ttt);
                        if (td.bits.flow_size != 0 || td.bits.hidden)
                            return false;
                        if (Buildings::findAtTile(ttt))
                            return false;
                    }
                }
                for (int16_t _x = -3; _x <= 3; _x++)
                {
                    for (int16_t _y = -4; _y <= 4; _y++)
                    {
                        if (!surface_tile_at(t.x + _x, t.y + _y, true).isValid())
                            return false;
                    }
                }
                return true;
            });

    if (!ent0.isValid())
    {
        if (!allow_ice)
        {
            allow_ice = true;

            return scan_fort_entrance(out);
        }

        ai->debug(out, "[ERROR] Can't find a fortress entrance spot. We need a 3x5 flat area with solid ground for at least 2 tiles on each side.");
        return CR_FAILURE;
    }

    df::coord ent = surface_tile_at(ent0.x, ent0.y);

    fort_entrance = new room(ent - df::coord(0, 1, 0), ent + df::coord(0, 1, 0), "main staircase - fort entrance");
    for (int i = 0; i < 3; i++)
    {
        fort_entrance->layout.push_back(new_cage_trap(i - 1, -1));
        fort_entrance->layout.push_back(new_cage_trap(1, i));
        fort_entrance->layout.push_back(new_cage_trap(1 - i, 3));
        fort_entrance->layout.push_back(new_cage_trap(-1, 2 - i));
    }
    for (int i = 0; i < 5; i++)
    {
        fort_entrance->layout.push_back(new_cage_trap(i - 2, -2));
        fort_entrance->layout.push_back(new_cage_trap(2, i - 1));
        fort_entrance->layout.push_back(new_cage_trap(2 - i, 4));
        fort_entrance->layout.push_back(new_cage_trap(-2, 3 - i));
    }

    return CR_OK;
}

// search how much we need to dig to find a spot for the full fortress body
// here we cheat and work as if the map was fully reveal()ed
command_result Plan::scan_fort_body(color_ostream & out)
{
    // use a hardcoded fort layout
    df::coord c = fort_entrance->pos();

    for (int16_t cz1 = c.z; cz1 >= 0; cz1--)
    {
        bool stop = false;
        // stop searching if we hit a cavern or an aquifer inside our main
        // staircase
        for (int16_t x = fort_entrance->min.x; !stop && x <= fort_entrance->max.x; x++)
        {
            for (int16_t y = fort_entrance->min.y; !stop && y <= fort_entrance->max.y; y++)
            {
                df::coord t(x, y, cz1 + MaxZ);
                if (!map_tile_nocavern(t) || Maps::getTileDesignation(t)->bits.water_table)
                    stop = true;
            }
        }
        if (stop)
        {
            break;
        }

        auto check = [this, c, cz1, &stop](int16_t dx, int16_t dy, int16_t dz)
        {
            df::coord t = df::coord(c, cz1) + df::coord(dx, dy, dz);
            df::tiletype tt = *Maps::getTileType(t);
            df::tiletype_material tm = ENUM_ATTR(tiletype, material, tt);
            if (ENUM_ATTR(tiletype, shape, tt) != tiletype_shape::WALL ||
                    Maps::getTileDesignation(t)->bits.water_table ||
                    (tm != tiletype_material::STONE &&
                     tm != tiletype_material::MINERAL &&
                     (!allow_ice || tm != tiletype_material::FROZEN_LIQUID) &&
                     (dz < 0 || (tm != tiletype_material::SOIL &&
                                 tm != tiletype_material::ROOT))))
                stop = true;
        };

        for (int16_t dz = MinZ; !stop && dz <= MaxZ; dz++)
        {
            // scan perimeter first to quickly eliminate caverns / bad rock
            // layers
            for (int16_t dx = MinX; !stop && dx <= MaxX; dx++)
            {
                for (int16_t dy = MinY; !stop && dy <= MaxY; dy += MaxY - MinY)
                {
                    check(dx, dy, dz);
                }
            }
            for (int16_t dx = MinX; !stop && dx <= MaxX; dx += MaxX - MinX)
            {
                for (int16_t dy = MinY + 1; !stop && dy <= MaxY - 1; dy++)
                {
                    check(dx, dy, dz);
                }
            }
            // perimeter ok, full scan
            for (int16_t dx = MinX + 1; !stop && dx <= MaxX - 1; dx++)
            {
                for (int16_t dy = MinY + 1; !stop && dy <= MaxY - 1; dy++)
                {
                    check(dx, dy, dz);
                }
            }
        }

        if (!stop)
        {
            fort_entrance->min.z = cz1;
            return CR_OK;
        }
    }

    ai->debug(out, "[ERROR] Too many caverns, cant find room for fort. We need more minerals!");
    return CR_FAILURE;
}

// assign rooms in the space found by scan_fort_*
command_result Plan::setup_blueprint_rooms(color_ostream & out)
{
    // hardcoded layout
    corridors.push_back(fort_entrance);

    df::coord f = fort_entrance->pos();

    command_result res;

    std::vector<room *> fe;
    fe.push_back(fort_entrance);

    f.z = fort_entrance->min.z;
    res = setup_blueprint_workshops(out, f, fe);
    if (res != CR_OK)
        return res;
    ai->debug(out, "workshop floor ready");

    fort_entrance->min.z--;
    f.z--;
    res = setup_blueprint_utilities(out, f, fe);
    if (res != CR_OK)
        return res;
    ai->debug(out, "utility floor ready");

    fort_entrance->min.z--;
    f.z--;
    res = setup_blueprint_stockpiles(out, f, fe);
    if (res != CR_OK)
        return res;
    ai->debug(out, "stockpile floor ready");

    for (size_t i = 0; i < 2; i++)
    {
        fort_entrance->min.z--;
        f.z--;
        res = setup_blueprint_bedrooms(out, f, fe, i);
        if (res != CR_OK)
            return res;
        ai->debug(out, stl_sprintf("bedroom floor ready %d/2", i + 1));
    }

    return CR_OK;
}

command_result Plan::setup_blueprint_workshops(color_ostream &, df::coord f, const std::vector<room *> & entr)
{
    room *corridor_center0 = new room(f + df::coord(-1, -1, 0), f + df::coord(-1, 1, 0), "main staircase - west workshops");
    corridor_center0->layout.push_back(new_door(-1, 0));
    corridor_center0->layout.push_back(new_door(-1, 2));
    corridor_center0->accesspath = entr;
    corridors.push_back(corridor_center0);

    room *corridor_center2 = new room(f + df::coord(1, -1, 0), f + df::coord(1, 1, 0), "main staircase - east workshops");
    corridor_center2->layout.push_back(new_door(1, 0));
    corridor_center2->layout.push_back(new_door(1, 2));
    corridor_center2->accesspath = entr;
    corridors.push_back(corridor_center2);

    // add minimal stockpile in front of workshop
    const static struct sptypes
    {
        std::map<std::string, std::tuple<std::string, std::set<df::stockpile_list>, bool, bool>> map;
        sptypes()
        {
            std::set<df::stockpile_list> disable;
            disable.insert(stockpile_list::StoneOres);
            disable.insert(stockpile_list::StoneClay);
            map["Masons"] = std::make_tuple("stone", disable, false, false);
            disable.clear();
            map["Carpenters"] = std::make_tuple("wood", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::RefuseItems);
            disable.insert(stockpile_list::RefuseCorpses);
            disable.insert(stockpile_list::RefuseParts);
            map["Craftsdwarfs"] = std::make_tuple("refuse", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::FoodMeat);
            disable.insert(stockpile_list::FoodFish);
            disable.insert(stockpile_list::FoodUnpreparedFish);
            disable.insert(stockpile_list::FoodEgg);
            disable.insert(stockpile_list::FoodDrinkPlant);
            disable.insert(stockpile_list::FoodDrinkAnimal);
            disable.insert(stockpile_list::FoodCheesePlant);
            disable.insert(stockpile_list::FoodCheeseAnimal);
            disable.insert(stockpile_list::FoodSeeds);
            disable.insert(stockpile_list::FoodLeaves);
            disable.insert(stockpile_list::FoodMilledPlant);
            disable.insert(stockpile_list::FoodBoneMeal);
            disable.insert(stockpile_list::FoodFat);
            disable.insert(stockpile_list::FoodPaste);
            disable.insert(stockpile_list::FoodPressedMaterial);
            disable.insert(stockpile_list::FoodExtractPlant);
            disable.insert(stockpile_list::FoodMiscLiquid);
            map["Farmers"] = std::make_tuple("food", disable, true, false);
            disable.clear();
            disable.insert(stockpile_list::FoodMeat);
            disable.insert(stockpile_list::FoodFish);
            disable.insert(stockpile_list::FoodEgg);
            disable.insert(stockpile_list::FoodPlants);
            disable.insert(stockpile_list::FoodDrinkPlant);
            disable.insert(stockpile_list::FoodDrinkAnimal);
            disable.insert(stockpile_list::FoodCheesePlant);
            disable.insert(stockpile_list::FoodCheeseAnimal);
            disable.insert(stockpile_list::FoodSeeds);
            disable.insert(stockpile_list::FoodLeaves);
            disable.insert(stockpile_list::FoodMilledPlant);
            disable.insert(stockpile_list::FoodBoneMeal);
            disable.insert(stockpile_list::FoodFat);
            disable.insert(stockpile_list::FoodPaste);
            disable.insert(stockpile_list::FoodPressedMaterial);
            disable.insert(stockpile_list::FoodExtractPlant);
            disable.insert(stockpile_list::FoodExtractAnimal);
            disable.insert(stockpile_list::FoodMiscLiquid);
            map["Fishery"] = std::make_tuple("food", disable, true, false);
            disable.clear();
            disable.insert(stockpile_list::FoodFish);
            disable.insert(stockpile_list::FoodUnpreparedFish);
            disable.insert(stockpile_list::FoodEgg);
            disable.insert(stockpile_list::FoodPlants);
            disable.insert(stockpile_list::FoodDrinkPlant);
            disable.insert(stockpile_list::FoodDrinkAnimal);
            disable.insert(stockpile_list::FoodCheesePlant);
            disable.insert(stockpile_list::FoodCheeseAnimal);
            disable.insert(stockpile_list::FoodSeeds);
            disable.insert(stockpile_list::FoodLeaves);
            disable.insert(stockpile_list::FoodMilledPlant);
            disable.insert(stockpile_list::FoodBoneMeal);
            disable.insert(stockpile_list::FoodFat);
            disable.insert(stockpile_list::FoodPaste);
            disable.insert(stockpile_list::FoodPressedMaterial);
            disable.insert(stockpile_list::FoodExtractPlant);
            disable.insert(stockpile_list::FoodExtractAnimal);
            disable.insert(stockpile_list::FoodMiscLiquid);
            map["Butchers"] = std::make_tuple("food", disable, true, false);
            disable.clear();
            map["Jewelers"] = std::make_tuple("gems", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::ClothSilk);
            disable.insert(stockpile_list::ClothPlant);
            disable.insert(stockpile_list::ClothYarn);
            disable.insert(stockpile_list::ClothMetal);
            map["Loom"] = std::make_tuple("cloth", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::ThreadSilk);
            disable.insert(stockpile_list::ThreadPlant);
            disable.insert(stockpile_list::ThreadYarn);
            disable.insert(stockpile_list::ThreadMetal);
            map["Clothiers"] = std::make_tuple("cloth", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::FoodMeat);
            disable.insert(stockpile_list::FoodFish);
            disable.insert(stockpile_list::FoodUnpreparedFish);
            disable.insert(stockpile_list::FoodEgg);
            disable.insert(stockpile_list::FoodDrinkPlant);
            disable.insert(stockpile_list::FoodDrinkAnimal);
            disable.insert(stockpile_list::FoodCheesePlant);
            disable.insert(stockpile_list::FoodCheeseAnimal);
            disable.insert(stockpile_list::FoodSeeds);
            disable.insert(stockpile_list::FoodMilledPlant);
            disable.insert(stockpile_list::FoodBoneMeal);
            disable.insert(stockpile_list::FoodFat);
            disable.insert(stockpile_list::FoodPaste);
            disable.insert(stockpile_list::FoodPressedMaterial);
            disable.insert(stockpile_list::FoodExtractPlant);
            disable.insert(stockpile_list::FoodExtractAnimal);
            disable.insert(stockpile_list::FoodMiscLiquid);
            map["Still"] = std::make_tuple("food", disable, true, false);
            disable.clear();
            disable.insert(stockpile_list::FoodDrinkPlant);
            disable.insert(stockpile_list::FoodDrinkAnimal);
            disable.insert(stockpile_list::FoodSeeds);
            disable.insert(stockpile_list::FoodBoneMeal);
            disable.insert(stockpile_list::FoodMiscLiquid);
            map["Kitchen"] = std::make_tuple("food", disable, true, false);
            disable.clear();
            map["WoodFurnace"] = std::make_tuple("wood", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::StoneOther);
            disable.insert(stockpile_list::StoneClay);
            map["Smelter"] = std::make_tuple("stone", disable, false, false);
            disable.clear();
            disable.insert(stockpile_list::BlocksStone);
            disable.insert(stockpile_list::BlocksMetal);
            disable.insert(stockpile_list::BlocksOther);
            map["MetalsmithsForge"] = std::make_tuple("bars_blocks", disable, false, false);
        }
    } sptypes;

    // Millstone, Siege, magma workshops/furnaces
    std::vector<std::string> types;
    types.push_back("Still");
    types.push_back("Kitchen");
    types.push_back("Fishery");
    types.push_back("Butchers");
    types.push_back("Leatherworks");
    types.push_back("Tanners");
    types.push_back("Loom");
    types.push_back("Clothiers");
    types.push_back("Dyers");
    types.push_back("Bowyers");
    types.push_back("");
    types.push_back("Kiln");
    types.push_back("Masons");
    types.push_back("Carpenters");
    types.push_back("Mechanics");
    types.push_back("Farmers");
    types.push_back("Craftsdwarfs");
    types.push_back("Jewelers");
    types.push_back("Smelter");
    types.push_back("MetalsmithsForge");
    types.push_back("Ashery");
    types.push_back("WoodFurnace");
    types.push_back("SoapMaker");
    types.push_back("GlassFurnace");
    auto type_it = types.begin();

    for (int16_t dirx = -1; dirx <= 1; dirx += 2)
    {
        room *prev_corx = dirx < 0 ? corridor_center0 : corridor_center2;
        int16_t ocx = f.x + dirx * 3;
        for (int16_t dx = 1; dx <= 6; dx++)
        {
            // segments of the big central horizontal corridor
            int16_t cx = f.x + dirx * 4 * dx;
            room *cor_x = new room(df::coord(ocx, f.y - 1, f.z), df::coord(cx + (dx <= 5 ? 0 : dirx), f.y + 1, f.z), stl_sprintf("%s workshops - segment %d", dirx == 1 ? "east" : "west", dx));
            cor_x->accesspath.push_back(prev_corx);
            corridors.push_back(cor_x);
            prev_corx = cor_x;
            ocx = cx + dirx;

            if (dirx == 1 && dx == 3)
            {
                // stuff a quern&screwpress near the farmers'
                df::coord c(cx - 2, f.y + 1, f.z);
                room *r = new room(room_type::workshop, "Quern", c, c);
                r->accesspath.push_back(cor_x);
                r->level = 0;
                rooms.push_back(r);

                c.x = cx - 6;
                r = new room(room_type::workshop, "Quern", c, c);
                r->accesspath.push_back(cor_x);
                r->level = 1;
                rooms.push_back(r);

                c.x = cx + 2;
                r = new room(room_type::workshop, "Quern", c, c);
                r->accesspath.push_back(cor_x);
                r->level = 2;
                rooms.push_back(r);

                c.x = cx - 2;
                c.y = f.y - 1;
                r = new room(room_type::workshop, "ScrewPress", c, c);
                r->accesspath.push_back(cor_x);
                r->level = 0;
                rooms.push_back(r);
            }
            else if (dirx == -1 && dx == 6)
            {
                room *r = new room(room_type::location, "library", df::coord(cx - 12, f.y - 5, f.z), df::coord(cx - 3, f.y - 1, f.z));
                r->layout.push_back(new_door(10, 4));
                r->layout.push_back(new_furniture("chest", 9, 3));
                r->layout.push_back(new_furniture("chest", 9, 2));
                r->layout.push_back(new_furniture("table", 9, 1));
                r->layout.push_back(new_furniture("chair", 8, 1));
                r->layout.push_back(new_furniture("table", 9, 0));
                r->layout.push_back(new_furniture("chair", 8, 0));
                for (int16_t i = 0; i < 6; i++)
                {
                    r->layout.push_back(new_furniture("bookcase", i + 1, 1));
                    r->layout.push_back(new_furniture("bookcase", i + 1, 3));
                }
                r->accesspath.push_back(cor_x);
                rooms.push_back(r);

                r = new room(room_type::location, "temple", df::coord(cx - 12, f.y + 1, f.z), df::coord(cx - 3, f.y + 5, f.z));
                r->layout.push_back(new_door(10, 0));
                r->accesspath.push_back(cor_x);
                rooms.push_back(r);
            }

            std::string t = *type_it++;

            room *r = new room(room_type::workshop, t, df::coord(cx - 1, f.y - 5, f.z), df::coord(cx + 1, f.y - 3, f.z));
            r->accesspath.push_back(cor_x);
            r->layout.push_back(new_door(1, 3));
            r->level = 0;
            if (dirx == -1 && dx == 1)
            {
                r->layout.push_back(new_furniture("nestbox", -1, 4));
            }
            rooms.push_back(r);

            if (sptypes.map.count(r->subtype))
            {
                auto stock = sptypes.map.at(r->subtype);
                room *sp = new room(room_type::stockpile, std::get<0>(stock), df::coord(r->min.x, r->max.y + 2, r->min.z), df::coord(r->max.x, r->max.y + 2, r->min.z));
                sp->stock_disable = std::get<1>(stock);
                sp->stock_specific1 = std::get<2>(stock);
                sp->stock_specific2 = std::get<3>(stock);
                sp->workshop = r;
                sp->level = 0;
                rooms.push_back(sp);
            }

            r = new room(room_type::workshop, t, df::coord(cx - 1, f.y - 8, f.z), df::coord(cx + 1, f.y - 6, f.z));
            r->accesspath.push_back(rooms.back());
            r->level = 1;
            rooms.push_back(r);

            r = new room(room_type::workshop, t, df::coord(cx - 1, f.y - 11, f.z), df::coord(cx + 1, f.y - 9, f.z));
            r->accesspath.push_back(rooms.back());
            r->level = 2;
            rooms.push_back(r);

            t = *type_it++;
            r = new room(room_type::workshop, t, df::coord(cx - 1, f.y + 3, f.z), df::coord(cx + 1, f.y + 5, f.z));
            r->accesspath.push_back(cor_x);
            r->layout.push_back(new_door(1, -1));
            r->level = 0;
            if (dirx == -1 && dx == 1)
            {
                r->layout.push_back(new_furniture("nestbox", -1, -2));
            }
            rooms.push_back(r);

            if (sptypes.map.count(r->subtype))
            {
                auto stock = sptypes.map.at(r->subtype);
                room *sp = new room(room_type::stockpile, std::get<0>(stock), df::coord(r->min.x, r->min.y - 2, r->min.z), df::coord(r->max.x, r->min.y - 2, r->min.z));
                sp->stock_disable = std::get<1>(stock);
                sp->stock_specific1 = std::get<2>(stock);
                sp->stock_specific2 = std::get<3>(stock);
                sp->workshop = r;
                sp->level = 0;
                rooms.push_back(sp);
            }

            r = new room(room_type::workshop, t, df::coord(cx - 1, f.y + 6, f.z), df::coord(cx + 1, f.y + 8, f.z));
            r->accesspath.push_back(rooms.back());
            r->level = 1;
            rooms.push_back(r);

            r = new room(room_type::workshop, t, df::coord(cx - 1, f.y + 9, f.z), df::coord(cx + 1, f.y + 11, f.z));
            r->accesspath.push_back(rooms.back());
            r->level = 2;
            rooms.push_back(r);
        }
    }

    df::coord depot_center = spiral_search(df::coord(f.x - 4, f.y, fort_entrance->max.z - 1), [this](df::coord t) -> bool
            {
                for (int16_t dx = -2; dx <= 2; dx++)
                {
                    for (int16_t dy = -2; dy <= 2; dy++)
                    {
                        df::coord tt = t + df::coord(dx, dy, 0);
                        if (!map_tile_in_rock(tt))
                            return false;
                        if (map_tile_intersects_room(tt))
                            return false;
                    }
                }
                for (int16_t dy = -1; dy <= 1; dy++)
                {
                    df::coord tt = t + df::coord(-3, dy, 0);
                    if (!map_tile_in_rock(tt))
                        return false;
                    df::coord ttt = tt + df::coord(0, 0, 1);
                    if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(ttt))) != tiletype_shape_basic::Floor)
                        return false;
                    if (map_tile_intersects_room(tt))
                        return false;
                    if (map_tile_intersects_room(ttt))
                        return false;
                    df::tile_occupancy *occ = Maps::getTileOccupancy(ttt);
                    if (occ && occ->bits.building != tile_building_occ::None)
                        return false;
                }
                return true;
            });

    if (depot_center.isValid())
    {
        room *r = new room(room_type::workshop, "TradeDepot", depot_center - df::coord(2,2, 0), depot_center + df::coord(2, 2, 0));
        r->level = 0;
        r->layout.push_back(new_dig(tile_dig_designation::Ramp, -1, 1));
        r->layout.push_back(new_dig(tile_dig_designation::Ramp, -1, 2));
        r->layout.push_back(new_dig(tile_dig_designation::Ramp, -1, 3));
        rooms.push_back(r);
    }
    else
    {
        room *r = new room(room_type::workshop, "TradeDepot", df::coord(f.x - 7, f.y - 2, fort_entrance->max.z), df::coord(f.x - 3, f.y + 2, fort_entrance->max.z));
        r->level = 0;
        for (int16_t x = 0; x < 5; x++)
        {
            for (int16_t y = 0; y < 5; y++)
            {
                r->layout.push_back(new_construction(construction_type::Floor, x, y));
            }
        }
        rooms.push_back(r);
    }
    return CR_OK;
}

command_result Plan::setup_blueprint_stockpiles(color_ostream & out, df::coord f, const std::vector<room *> & entr)
{
    room *corridor_center0 = new room(f + df::coord(-1, -1, 0), f + df::coord(-1, 1, 0), "main staircase - west stockpiles");
    corridor_center0->layout.push_back(new_door(-1, 0));
    corridor_center0->layout.push_back(new_door(-1, 2));
    corridor_center0->accesspath = entr;
    corridors.push_back(corridor_center0);

    room *corridor_center2 = new room(f + df::coord(1, -1, 0), f + df::coord(1, 1, 0), "main staircase - east stockpiles");
    corridor_center2->layout.push_back(new_door(1, 0));
    corridor_center2->layout.push_back(new_door(1, 2));
    corridor_center2->accesspath = entr;
    corridors.push_back(corridor_center2);

    std::vector<std::string> types;
    types.push_back("food");
    types.push_back("furniture");
    types.push_back("wood");
    types.push_back("stone");
    types.push_back("refuse");
    types.push_back("animals");
    types.push_back("corpses");
    types.push_back("gems");
    types.push_back("finished_goods");
    types.push_back("cloth");
    types.push_back("bars_blocks");
    types.push_back("leather");
    types.push_back("ammo");
    types.push_back("armor");
    types.push_back("weapons");
    types.push_back("coins");
    auto type_it = types.begin();

    // TODO side stairs to workshop level ?
    for (int16_t dirx = -1; dirx <= 1; dirx += 2)
    {
        room *prev_corx = dirx < 0 ? corridor_center0 : corridor_center2;
        int16_t ocx = f.x + dirx * 3;
        for (int16_t dx = 1; dx <= 4; dx++)
        {
            // segments of the big central horizontal corridor
            int16_t cx = f.x + dirx * (8 * dx - 4);
            room *cor_x = new room(df::coord(ocx, f.y - 1, f.z), df::coord(cx + dirx, f.y + 1, f.z), stl_sprintf("%s stockpiles - segment %d", dirx == 1 ? "east" : "west", dx));
            cor_x->accesspath.push_back(prev_corx);
            corridors.push_back(cor_x);
            prev_corx = cor_x;
            ocx = cx + 2 * dirx;

            std::string t0 = *type_it++;
            std::string t1 = *type_it++;
            room *r0 = new room(room_type::stockpile, t0, df::coord(cx - 3, f.y - 4, f.z), df::coord(cx + 3, f.y - 3, f.z));
            room *r1 = new room(room_type::stockpile, t1, df::coord(cx - 3, f.y + 3, f.z), df::coord(cx + 3, f.y + 4, f.z));
            r0->level = 1;
            r1->level = 1;
            r0->accesspath.push_back(cor_x);
            r1->accesspath.push_back(cor_x);
            r0->layout.push_back(new_door(2, 2));
            r0->layout.push_back(new_door(4, 2));
            r1->layout.push_back(new_door(2, -1));
            r1->layout.push_back(new_door(4, -1));
            rooms.push_back(r0);
            rooms.push_back(r1);

            r0 = new room(room_type::stockpile, t0, df::coord(cx - 3, f.y - 11, f.z), df::coord(cx + 3, f.y - 5, f.z));
            r1 = new room(room_type::stockpile, t1, df::coord(cx - 3, f.y + 5, f.z), df::coord(cx + 3, f.y + 11, f.z));
            r0->level = 2;
            r1->level = 2;
            rooms.push_back(r0);
            rooms.push_back(r1);

            r0 = new room(room_type::stockpile, t0, df::coord(cx - 3, f.y - 20, f.z), df::coord(cx + 3, f.y - 12, f.z));
            r1 = new room(room_type::stockpile, t1, df::coord(cx - 3, f.y + 12, f.z), df::coord(cx + 3, f.y + 20, f.z));
            r0->level = 3;
            r1->level = 3;
            rooms.push_back(r0);
            rooms.push_back(r1);
        }
    }
    for (auto it = rooms.begin(); it != rooms.end(); it++)
    {
        room *r = *it;
        if (r->type == room_type::stockpile && r->subtype == "coins" &&
                r->level > 1)
        {
            r->subtype = "furniture";
            r->level += 2;
        }
    }

    return setup_blueprint_pitcage(out);
}

command_result Plan::setup_blueprint_pitcage(color_ostream &)
{
    room *gpit = find_room(room_type::garbagepit);
    if (!gpit)
        return CR_OK;
    auto layout = [](room *r)
    {
        r->layout.push_back(new_construction(construction_type::UpStair, -1, 1, -10));
        for (int16_t z = -9; z <= -1; z++)
        {
            r->layout.push_back(new_construction(construction_type::UpDownStair, -1, 1, z));
        }
        r->layout.push_back(new_construction(construction_type::DownStair, -1, 1, 0));
        std::vector<int16_t> dxs;
        dxs.push_back(0);
        dxs.push_back(1);
        dxs.push_back(2);
        std::vector<int16_t> dys;
        dys.push_back(1);
        dys.push_back(0);
        dys.push_back(2);
        for (auto dx = dxs.begin(); dx != dxs.end(); dx++)
        {
            for (auto dy = dys.begin(); dy != dys.end(); dy++)
            {
                if (*dx == 1 && *dy == 1)
                {
                    r->layout.push_back(new_dig(tile_dig_designation::Channel, *dx, *dy));
                }
                else
                {
                    r->layout.push_back(new_construction(construction_type::Floor, *dx, *dy));
                }
            }
        }
    };

    room *r = new room(room_type::pitcage, "", gpit->min + df::coord(-1, -1, 10), gpit->min + df::coord(1, 1, 10));
    layout(r);
    r->layout.push_back(new_hive_floor(3, 1));
    rooms.push_back(r);

    room *stockpile = new room(room_type::stockpile, "animals", r->min, r->max, "pitting queue");
    stockpile->level = 0;
    stockpile->stock_specific1 = true; // no empty cages
    stockpile->stock_specific2 = true; // no empty animal traps
    layout(stockpile);
    rooms.push_back(stockpile);

    return CR_OK;
}

command_result Plan::setup_blueprint_utilities(color_ostream & out, df::coord f, const std::vector<room *> & entr)
{
    room *corridor_center0 = new room(f + df::coord(-1, -1, 0), f + df::coord(-1, 1, 0), "main staircase - west utilities");
    corridor_center0->layout.push_back(new_door(-1, 0));
    corridor_center0->layout.push_back(new_door(-1, 2));
    corridor_center0->accesspath = entr;
    corridors.push_back(corridor_center0);

    room *corridor_center2 = new room(f + df::coord(1, -1, 0), f + df::coord(1, 1, 0), "main staircase - east utilities");
    corridor_center2->layout.push_back(new_door(1, 0));
    corridor_center2->layout.push_back(new_door(1, 2));
    corridor_center2->accesspath = entr;
    corridors.push_back(corridor_center2);

    // dining halls
    int16_t ocx = f.x - 2;
    room *old_cor = corridor_center0;

    // temporary dininghall, for fort start
    room *tmp = new room(room_type::dininghall, "", f + df::coord(-4, -1, 0), f + df::coord(-3, 1, 0), "temporary dining room");
    tmp->temporary = true;
    for (int16_t dy = 0; dy <= 2; dy++)
    {
        tmp->layout.push_back(new_furniture_with_users("table", 0, dy));
        tmp->layout.push_back(new_furniture_with_users("chair", 1, dy));
    }
    tmp->layout[0]->makeroom = true;
    tmp->accesspath.push_back(old_cor);
    rooms.push_back(tmp);

    // dininghalls x 4 (54 users each)
    for (int16_t ax = 0; ax <= 1; ax++)
    {
        room *cor = new room(df::coord(ocx - 1, f.y - 1, f.z), df::coord(f.x - ax * 12 - 5, f.y + 1, f.z), stl_sprintf("west utilities - segment %d", ax + 1));
        cor->accesspath.push_back(old_cor);
        corridors.push_back(cor);
        ocx = f.x - ax * 12 - 4;
        old_cor = cor;

        std::vector<int16_t> dxs;
        dxs.push_back(5);
        dxs.push_back(4);
        dxs.push_back(6);
        dxs.push_back(3);
        dxs.push_back(7);
        dxs.push_back(2);
        dxs.push_back(8);
        dxs.push_back(1);
        dxs.push_back(9);
        for (int16_t dy = -1; dy <= 1; dy += 2)
        {
            room *dinner = new room(room_type::dininghall, "", df::coord(f.x - ax * 12 - 2 - 10, f.y + dy * 9, f.z), df::coord(f.x - ax * 12 - 2, f.y + dy * 3, f.z), stl_sprintf("dining room %c", 'A' + ax + dy + 1));
            dinner->layout.push_back(new_door(7, dy > 0 ? -1 : 7));
            dinner->layout.push_back(new_wall(2, 3));
            dinner->layout.push_back(new_wall(8, 3));
            for (auto dx = dxs.begin(); dx != dxs.end(); dx++)
            {
                for (int16_t sy = -1; sy <= 1; sy += 2)
                {
                    dinner->layout.push_back(new_furniture_with_users("table", *dx, 3 + dy * sy * 1, true));
                    dinner->layout.push_back(new_furniture_with_users("chair", *dx, 3 + dy * sy * 2, true));
                }
            }
            for (auto f = dinner->layout.begin(); f != dinner->layout.end(); f++)
            {
                if ((*f)->item == "table")
                {
                    (*f)->makeroom = true;
                    break;
                }
            }
            dinner->accesspath.push_back(cor);
            rooms.push_back(dinner);
        }
    }

    // tavern
    room *cor = new room(f + df::coord(-18, -1, 0), f + df::coord(-26, 1, 0), "tavern entrance");
    cor->accesspath.push_back(corridors.back());
    corridors.push_back(cor);

    df::coord tavern_center = f - df::coord(32, 0, 0);
    room *tavern = new room(room_type::location, "tavern", tavern_center - df::coord(4, 4, 0), tavern_center + df::coord(4, 4, 0));
    tavern->layout.push_back(new_door(9, 3));
    tavern->layout.push_back(new_door(9, 5));
    tavern->layout.push_back(new_furniture("chest", 8, 0));
    tavern->accesspath.push_back(cor);
    rooms.push_back(tavern);

    room *booze = new room(room_type::stockpile, "food", tavern_center + df::coord(-2, -4, 0), tavern_center + df::coord(3, -4, 0), "tavern booze");
    booze->stock_disable.insert(stockpile_list::FoodMeat);
    booze->stock_disable.insert(stockpile_list::FoodFish);
    booze->stock_disable.insert(stockpile_list::FoodUnpreparedFish);
    booze->stock_disable.insert(stockpile_list::FoodEgg);
    booze->stock_disable.insert(stockpile_list::FoodPlants);
    booze->stock_disable.insert(stockpile_list::FoodCheesePlant);
    booze->stock_disable.insert(stockpile_list::FoodCheeseAnimal);
    booze->stock_disable.insert(stockpile_list::FoodSeeds);
    booze->stock_disable.insert(stockpile_list::FoodLeaves);
    booze->stock_disable.insert(stockpile_list::FoodMilledPlant);
    booze->stock_disable.insert(stockpile_list::FoodBoneMeal);
    booze->stock_disable.insert(stockpile_list::FoodFat);
    booze->stock_disable.insert(stockpile_list::FoodPaste);
    booze->stock_disable.insert(stockpile_list::FoodPressedMaterial);
    booze->stock_disable.insert(stockpile_list::FoodExtractPlant);
    booze->stock_disable.insert(stockpile_list::FoodExtractAnimal);
    booze->stock_disable.insert(stockpile_list::FoodMiscLiquid);
    booze->stock_specific1 = true; // no prepared food
    booze->workshop = tavern;
    booze->level = 0;
    tavern->accesspath.push_back(booze);
    rooms.push_back(booze);

    if (allow_ice)
    {
        ai->debug(out, "icy embark, no well");
        booze->min.x -= 2;
    }
    else
    {
        df::coord river = scan_river(out);
        if (river.isValid())
        {
            command_result res = setup_blueprint_cistern_fromsource(out, river, f, tavern);
            if (res != CR_OK)
                return res;
        }
        else
        {
            // TODO pool, pumps, etc
            ai->debug(out, "no river, no well");
            booze->min.x -= 2;
        }
    }

    // farm plots
    int16_t cx = f.x + 4 * 6; // end of workshop corridor (last ws door)
    int16_t cy = f.y;
    int16_t cz = find_room(room_type::workshop, [](room *r) -> bool { return r->subtype == "Farmers"; })->min.z;
    room *ws_cor = new room(df::coord(f.x + 3, cy, cz), df::coord(cx + 1, cy, cz), "farm access corridor"); // ws_corr->accesspath ...
    corridors.push_back(ws_cor);
    room *farm_stairs = new room(df::coord(cx + 2, cy, cz), df::coord(cx + 2, cy, cz), "farm stairs");
    farm_stairs->accesspath.push_back(ws_cor);
    corridors.push_back(farm_stairs);
    cx += 3;
    int16_t cz2 = cz;
    int32_t soilcnt = 0;
    for (int16_t z = cz; z < world->map.z_count; z++)
    {
        bool ok = true;
        int32_t scnt = 0;
        for (int16_t dx = -1; dx <= nrfarms * farm_w / 3; dx++)
        {
            for (int16_t dy = -3 * farm_h - farm_h + 1; dy <= 3 * farm_h + farm_h - 1; dy++)
            {
                df::tiletype *t = Maps::getTileType(cx + dx, cy + dy, z);
                if (!t || ENUM_ATTR(tiletype, shape, *t) != tiletype_shape::WALL)
                {
                    ok = false;
                    continue;
                }

                if (ENUM_ATTR(tiletype, material, *t) == tiletype_material::SOIL)
                {
                    scnt++;
                }
            }
        }

        if (ok && soilcnt < scnt)
        {
            soilcnt = scnt;
            cz2 = z;
        }
    }

    farm_stairs->max.z = cz2;
    cor = new room(df::coord(cx, cy, cz2), df::coord(cx + 1, cy, cz2), "farm corridor");
    cor->accesspath.push_back(farm_stairs);
    corridors.push_back(cor);
    room *first_farm = nullptr;
    auto make_farms = [this, cor, cx, cy, cz2, &first_farm](int16_t dy, std::string st)
    {
        for (int16_t dx = 0; dx < nrfarms / 3; dx++)
        {
            for (int16_t ddy = 0; ddy < 3; ddy++)
            {
                room *r = new room(room_type::farmplot, st, df::coord(cx + farm_w * dx, cy + dy * 2 + dy * ddy * farm_h, cz2), df::coord(cx + farm_w * dx + farm_w - 1, cy + dy * (2 + farm_h - 1) + dy * ddy * farm_h, cz2));
                r->has_users = true;
                if (dx == 0 && ddy == 0)
                {
                    r->layout.push_back(new_door(1, dy > 0 ? -1 : farm_h));
                    r->accesspath.push_back(cor);
                }
                else
                {
                    r->accesspath.push_back(rooms.back());
                }
                rooms.push_back(r);
                if (first_farm == nullptr)
                {
                    first_farm = r;
                }
            }
        }
    };
    make_farms(-1, "food");
    make_farms(1, "cloth");

    // seeds stockpile
    room *r = new room(room_type::stockpile, "food", df::coord(cx + 2, cy, cz2), df::coord(cx + 4, cy, cz2), "farm seeds stockpile");
    r->level = 0;
    r->stock_disable.insert(stockpile_list::FoodMeat);
    r->stock_disable.insert(stockpile_list::FoodFish);
    r->stock_disable.insert(stockpile_list::FoodUnpreparedFish);
    r->stock_disable.insert(stockpile_list::FoodEgg);
    r->stock_disable.insert(stockpile_list::FoodPlants);
    r->stock_disable.insert(stockpile_list::FoodDrinkPlant);
    r->stock_disable.insert(stockpile_list::FoodDrinkAnimal);
    r->stock_disable.insert(stockpile_list::FoodCheesePlant);
    r->stock_disable.insert(stockpile_list::FoodCheeseAnimal);
    r->stock_disable.insert(stockpile_list::FoodLeaves);
    r->stock_disable.insert(stockpile_list::FoodMilledPlant);
    r->stock_disable.insert(stockpile_list::FoodBoneMeal);
    r->stock_disable.insert(stockpile_list::FoodFat);
    r->stock_disable.insert(stockpile_list::FoodPaste);
    r->stock_disable.insert(stockpile_list::FoodPressedMaterial);
    r->stock_disable.insert(stockpile_list::FoodExtractPlant);
    r->stock_disable.insert(stockpile_list::FoodExtractAnimal);
    r->stock_disable.insert(stockpile_list::FoodMiscLiquid);
    r->stock_specific1 = true; // no prepared meals
    r->workshop = first_farm;
    r->accesspath.push_back(cor);
    rooms.push_back(r);

    // garbage dump
    // TODO ensure flat space, no pools/tree, ...
    df::coord tile(cx + 5, cy, cz);
    r = new room(room_type::garbagedump, "", tile, tile);
    tile = spiral_search(tile, [this, f, cx, cz](df::coord t) -> bool
            {
                t = surface_tile_at(t.x, t.y);
                if (!t.isValid())
                    return false;
                if (t.x < cx + 5 && (t.z <= cz + 2 || t.x <= f.x + 5))
                    return false;
                if (!map_tile_in_rock(t + df::coord(0, 0, -1)))
                    return false;
                if (!map_tile_in_rock(t + df::coord(2, 0, -1)))
                    return false;
                if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(t))) != tiletype_shape_basic::Floor)
                    return false;
                if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(t + df::coord(1, 0, 0)))) != tiletype_shape_basic::Floor)
                    return false;
                if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(t + df::coord(2, 0, 0)))) != tiletype_shape_basic::Floor)
                    return false;
                if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(t + df::coord(1, 1, 0)))) == tiletype_shape_basic::Floor)
                    return true;
                if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *Maps::getTileType(t + df::coord(1, -1, 0)))) == tiletype_shape_basic::Floor)
                    return true;
                return false;
            });
    tile = surface_tile_at(tile.x, tile.y);
    r->min = r->max = tile;
    rooms.push_back(r);
    r = new room(room_type::garbagepit, "", tile + df::coord(1, 0, 0), tile + df::coord(2, 0, 0));
    r->layout.push_back(new_dig(tile_dig_designation::Channel, 0, 0));
    r->layout.push_back(new_dig(tile_dig_designation::Channel, 1, 0));
    rooms.push_back(r);

    // infirmary
    old_cor = corridor_center2;
    cor = new room(f + df::coord(3, -1, 0), f + df::coord(5, 1, 0), "east utilities - infirmary access");
    cor->accesspath.push_back(old_cor);
    corridors.push_back(cor);
    old_cor = cor;

    room *infirmary = new room(room_type::infirmary, "", f + df::coord(2, -3, 0), f + df::coord(6, -7, 0));
    infirmary->layout.push_back(new_door(3, 5));
    infirmary->layout.push_back(new_furniture("bed", 0, 1));
    infirmary->layout.push_back(new_furniture("table", 1, 1));
    infirmary->layout.push_back(new_furniture("bed", 2, 1));
    infirmary->layout.push_back(new_furniture("traction_bench", 0, 2));
    infirmary->layout.push_back(new_furniture("traction_bench", 2, 2));
    infirmary->layout.push_back(new_furniture("bed", 0, 3));
    infirmary->layout.push_back(new_furniture("table", 1, 3));
    infirmary->layout.push_back(new_furniture("bed", 2, 3));
    infirmary->layout.push_back(new_furniture("chest", 4, 1));
    infirmary->layout.push_back(new_furniture("chest", 4, 2));
    infirmary->layout.push_back(new_furniture("chest", 4, 3));
    infirmary->accesspath.push_back(cor);
    rooms.push_back(infirmary);

    // cemetary lots (160 spots)
    cor = new room(f + df::coord(6, -1, 0), f + df::coord(14, 1, 0), "east utilities - cemetary access");
    cor->accesspath.push_back(old_cor);
    corridors.push_back(cor);
    old_cor = cor;

    for (int16_t ry = 0; ry < 500; ry++)
    {
        bool stop = false;
        for (int16_t tx = -1; !stop && tx <= 19; tx++)
        {
            for (int16_t ty = -1; !stop && ty <= 4; ty++)
            {
                df::tiletype *t = Maps::getTileType(f + df::coord(10 + tx, -3 - 3 * ry - ty, 0));
                if (!t || ENUM_ATTR(tiletype_shape, basic_shape,
                            ENUM_ATTR(tiletype, shape, *t)) !=
                        tiletype_shape_basic::Wall)
                {
                    stop = true;
                }
            }
        }
        if (stop)
            break;

        for (int16_t rrx = 0; rrx < 2; rrx++)
        {
            for (int16_t rx = 0; rx < 2; rx++)
            {
                df::coord o = f + df::coord(10 + 5 * rx + 9 * rrx, -3 - 3 * ry, 0);
                room *cemetary = new room(room_type::cemetary, "", o, o + df::coord(4, -3, 0));
                for (int16_t dx = 0; dx < 4; dx++)
                {
                    for (int16_t dy = 0; dy < 2; dy++)
                    {
                        cemetary->layout.push_back(new_furniture_with_users("coffin", dx + 1 - rx, dy + 1, true));
                    }
                }
                if (rx == 0 && ry == 0 && rrx == 0)
                {
                    cemetary->layout.push_back(new_door(4, 4));
                    cemetary->accesspath.push_back(cor);
                }
                rooms.push_back(cemetary);
            }
        }
    }

    // barracks
    // 8 dwarf per squad, 20% pop => 40 soldiers for 200 dwarves => 5 barracks
    old_cor = corridor_center2;
    int16_t oldcx = old_cor->max.x + 2; // door
    for (int16_t rx = 0; rx < 4; rx++)
    {
        cor = new room(df::coord(oldcx, f.y - 1, f.z), df::coord(f.x + 5 + 10 * rx, f.y + 1, f.z), stl_sprintf("east utilities - segment %d", rx + 1));
        cor->accesspath.push_back(old_cor);
        corridors.push_back(cor);
        old_cor = cor;
        oldcx = cor->max.x + 1;

        for (int16_t ry = -1; ry <= 1; ry += 2)
        {
            if (ry == -1 && rx < 3) // infirmary/cemetary
                continue;

            room *barracks = new room(room_type::barracks, "", df::coord(f.x + 2 + 10 * rx, f.y + 3 * ry, f.z), df::coord(f.x + 2 + 10 * rx + 6, f.y + 10 * ry, f.z), stl_sprintf("barracks %c", ry == 1 ? 'B' + rx : 'A'));
            barracks->layout.push_back(new_door(3, ry > 0 ? -1 : 8));
            for (int16_t dy_ = 0; dy_ < 8; dy_++)
            {
                int16_t dy = ry < 0 ? 7 - dy_ : dy_;
                barracks->layout.push_back(new_furniture_with_users("armorstand", 5, dy, true));
                barracks->layout.push_back(new_furniture_with_users("bed", 6, dy, true));
                barracks->layout.push_back(new_furniture_with_users("cabinet", 0, dy, true));
                barracks->layout.push_back(new_furniture_with_users("chest", 1, dy, true));
            }
            barracks->layout.push_back(new_furniture_with_users("weaponrack", 4, ry > 0 ? 7 : 0, false));
            barracks->layout.back()->makeroom = true;
            barracks->layout.push_back(new_furniture_with_users("weaponrack", 2, ry > 0 ? 7 : 0, true));
            barracks->layout.push_back(new_furniture_with_users("archerytarget", 3, ry > 0 ? 7 : 0, true));
            barracks->accesspath.push_back(cor);
            rooms.push_back(barracks);
        }
    }

    ai->debug(out, "finished interior utilities");
    command_result res;
    res = setup_blueprint_pastures(out);
    if (res != CR_OK)
        return res;
    ai->debug(out, "finished pastures");
    res = setup_blueprint_outdoor_farms(out, nrfarms * 2);
    if (res != CR_OK)
        return res;
    return CR_OK;
    ai->debug(out, "finished outdoor farms");
}

command_result Plan::setup_blueprint_cistern_fromsource(color_ostream & out, df::coord src, df::coord f, room *tavern)
{
    // TODO dynamic layout, at least move the well/cistern on the side of the river
    // TODO scan for solid ground for all this

    // add a well to the tavern
    tavern->layout.push_back(new_well(4, 4));
    tavern->layout.push_back(new_cistern_lever(1, 0, "out"));
    tavern->layout.push_back(new_cistern_lever(0, 0, "in"));

    df::coord c = tavern->pos();

    // water cistern under the well (in the hole of bedroom blueprint)
    std::vector<room *> cist_cors = find_corridor_tosurface(out, c - df::coord(8, 0, 0));
    cist_cors.at(0)->min.z -= 3;

    room *cistern = new room(room_type::cistern, "well", c + df::coord(-7, -1, -3), c + df::coord(1, 1, -1));
    cistern->accesspath.push_back(cist_cors.at(0));

    // handle low rivers / high mountain forts
    if (f.z > src.z)
        f.z = c.z = src.z;
    // should be fine with cistern auto-fill checks
    if (cistern->min.z > f.z)
        cistern->min.z = f.z;

    // staging reservoir to fill the cistern, one z-level at a time
    // should have capacity = 1 cistern level @7/7 + itself @1/7 (rounded up)
    //  cistern is 9x3 + 1 (stairs)
    //  reserve is 5x7 (can fill cistern 7/7 + itself 1/7 + 14 spare
    room *reserve = new room(room_type::cistern, "reserve", c + df::coord(-10, -3, 0), c + df::coord(-14, 3, 0));
    reserve->layout.push_back(new_cistern_floodgate(-1, 3, "in", false));
    reserve->layout.push_back(new_cistern_floodgate(5, 3, "out", true));
    reserve->accesspath.push_back(cist_cors.at(0));

    // cisterns are dug in order
    // try to dig reserve first, so we have liquid water even if river freezes
    rooms.push_back(reserve);
    rooms.push_back(cistern);

    // link the cistern reserve to the water source

    // trivial walk of the river tiles to find a spot closer to dst
    auto move_river = [this, &src](df::coord dst)
    {
        auto distance = [](df::coord a, df::coord b) -> int16_t
        {
            df::coord d = a - b;
            return d.x * d.x + d.y * d.y + d.z * d.z;
        };

        df::coord nsrc = src;
        for (size_t i = 0; i < 500; i++)
        {
            if (!nsrc.isValid())
                break;
            src = nsrc;
            int16_t dist = distance(src, dst);
            nsrc = spiral_search(src, 1, 1, [distance, dist, dst](df::coord t) -> bool
                    {
                        if (distance(t, dst) > dist)
                            return false;
                        return Maps::getTileDesignation(t)->bits.feature_local;
                    });
        }
    };

    // 1st end: reservoir input
    df::coord p1 = c - df::coord(16, 0, 0);
    move_river(p1);
    ai->debug(out, stl_sprintf("cistern: reserve/in (%d, %d, %d), river (%d, %d, %d)", p1.x, p1.y, p1.z, src.x, src.y, src.z));

    df::coord p = p1;
    room *r = reserve;
    // XXX hardcoded layout again
    if (src.x > p1.x)
    {
        // the tunnel should walk around other blueprint rooms
        df::coord p2 = p1 + df::coord(0, src.y >= p1.y ? 26 : -26, 0);
        room *cor = new room(p1, p2);
        corridors.push_back(cor);
        reserve->accesspath.push_back(cor);
        move_river(p2);
        p = p2;
        r = cor;
    }

    std::vector<room *> up = find_corridor_tosurface(out, p);
    r->accesspath.push_back(up.at(0));

    df::coord dst = up.back()->max - df::coord(0, 0, 2);
    if (src.z <= dst.z)
        dst.z = src.z - 1;
    move_river(dst);

    if (std::abs(dst.x - src.x) > 1)
    {
        df::coord p3(src.x, dst.y, dst.z);
        move_river(p3);
    }

    // find safe tile near the river and a tile to channel
    df::coord channel;
    channel.clear();
    df::coord output = spiral_search(src, [this, &channel](df::coord t) -> bool
            {
                if (!map_tile_in_rock(t))
                {
                    return false;
                }
                channel = Plan::spiral_search(t, 1, 1, [](df::coord t) -> bool
                        {
                            return Plan::spiral_search(t, 1, 1, [](df::coord t) -> bool
                                    {
                                        return Maps::getTileDesignation(t)->bits.feature_local;
                                    }).isValid();
                        });
                if (!channel.isValid())
                {
                    channel = Plan::spiral_search(t, 1, 1, [](df::coord t) -> bool
                            {
                                return Plan::spiral_search(t, 1, 1, [](df::coord t) -> bool
                                        {
                                            return Maps::getTileDesignation(t)->bits.flow_size != 0 ||
                                                    ENUM_ATTR(tiletype, material, *Maps::getTileType(t)) == tiletype_material::FROZEN_LIQUID;
                                        }).isValid();
                            });
                }
                return channel.isValid();
            });

    if (channel.isValid())
    {
        ai->debug(out, stl_sprintf("cistern: out(%d, %d, %d), channel_enable (%d, %d, %d)", output.x, output.y, output.z, channel.x, channel.y, channel.z));
    }

    // TODO check that 'channel' is easily channelable (eg river in a hole)

    int16_t y_x = 0;
    if (dst.x - 1 > output.x)
    {
        room *cor = new room(df::coord(dst.x - 1, dst.y, dst.z), df::coord(output.x + 1, dst.y, dst.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
        r = cor;
        y_x = 1;
    }
    else if (output.x - 1 > dst.x)
    {
        room *cor = new room(df::coord(dst.x + 1, dst.y, dst.z), df::coord(output.x - 1, dst.y, dst.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
        r = cor;
        y_x = -1;
    }

    if (dst.y - 1 > output.y)
    {
        room *cor = new room(df::coord(output.x + y_x, dst.y + 1, dst.z), df::coord(output.x + y_x, output.y, dst.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
    }
    else if (output.y - 1 > dst.y)
    {
        room *cor = new room(df::coord(output.x + y_x, dst.y - 1, dst.z), df::coord(output.x + y_x, output.y, dst.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
    }

    up = find_corridor_tosurface(out, df::coord(output, dst.z));
    r->accesspath.push_back(up.at(0));

    reserve->channel_enable = channel;
    return CR_OK;
}

// scan for 11x11 flat areas with grass
command_result Plan::setup_blueprint_pastures(color_ostream & out)
{
    size_t want = 36;
    spiral_search(fort_entrance->pos(), std::max(world->map.x_count, world->map.y_count), 10, 5, [this, &out, &want](df::coord _t) -> bool
            {
                df::coord sf = surface_tile_at(_t.x, _t.y);
                if (!sf.isValid())
                    return false;
                size_t floortile = 0;
                size_t grasstile = 0;
                bool ok = true;
                for (int16_t dx = -5; ok && dx <= 5; dx++)
                {
                    for (int16_t dy = -5; ok && dy <= 5; dy++)
                    {
                        df::coord t = sf + df::coord(dx, dy, 0);
                        df::tiletype *tt = Maps::getTileType(t);
                        if (!tt)
                        {
                            ok = false;
                            continue;
                        }
                        if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *tt)) != tiletype_shape_basic::Floor && ENUM_ATTR(tiletype, material, *tt) != tiletype_material::TREE)
                        {
                            ok = false;
                            continue;
                        }
                        if (Maps::getTileDesignation(t)->bits.flow_size != 0)
                        {
                            continue;
                        }
                        if (ENUM_ATTR(tiletype, material, *tt) == tiletype_material::FROZEN_LIQUID)
                        {
                            continue;
                        }
                        floortile++;
                        auto & events = Maps::getTileBlock(t)->block_events;
                        for (auto be = events.begin(); be != events.end(); be++)
                        {
                            df::block_square_event_grassst *grass = virtual_cast<df::block_square_event_grassst>(*be);
                            if (grass && grass->amount[t.x & 0xf][t.y & 0xf] > 0)
                            {
                                grasstile++;
                                break;
                            }
                        }
                    }
                }
                if (ok && floortile >= 9 * 9 && grasstile >= 8 * 8)
                {
                    room *r = new room(room_type::pasture, "", sf - df::coord(5, 5, 0), sf + df::coord(5, 5, 0));
                    r->has_users = true;
                    rooms.push_back(r);
                    want--;
                }
                return want == 0;
            });
    return CR_OK;
}

// scan for 3x3 flat areas with soil
command_result Plan::setup_blueprint_outdoor_farms(color_ostream & out, size_t want)
{
    spiral_search(fort_entrance->pos(), std::max(world->map.x_count, world->map.y_count), 9, 3, [this, &out, &want](df::coord _t) -> bool
            {
                df::coord sf = surface_tile_at(_t.x, _t.y);
                if (!sf.isValid())
                    return false;
                df::tile_designation sd = *Maps::getTileDesignation(sf);
                for (int16_t dx = -1; dx <= 1; dx++)
                {
                    for (int16_t dy = -1; dy <= 1; dy++)
                    {
                        df::coord t = sf + df::coord(dx, dy, 0);
                        df::tile_designation *td = Maps::getTileDesignation(t);
                        if (!td)
                        {
                            return false;
                        }
                        if (sd.bits.subterranean != td->bits.subterranean)
                        {
                            return false;
                        }
                        if (!sd.bits.subterranean &&
                                sd.bits.biome != td->bits.biome)
                        {
                            return false;
                        }
                        df::tiletype tt = *Maps::getTileType(t);
                        if (ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, tt)) != tiletype_shape_basic::Floor)
                        {
                            return false;
                        }
                        if (td->bits.flow_size != 0)
                        {
                            return false;
                        }
                        if (!farm_allowed_materials.set.count(ENUM_ATTR(tiletype, material, tt)))
                        {
                            return false;
                        }
                    }
                }
                room *r = new room(room_type::farmplot, want % 2 == 0 ? "food" : "cloth", sf - df::coord(1, 1, 0), sf + df::coord(1, 1, 0));
                r->has_users = true;
                r->outdoor = true;
                want--;
                return want == 0;
            });
    return CR_OK;
}

command_result Plan::setup_blueprint_bedrooms(color_ostream &, df::coord f, const std::vector<room *> & entr, int level)
{
    room *corridor_center0 = new room(f + df::coord(-1, -1, 0), f + df::coord(-1, 1, 0), stl_sprintf("main staircase - west %s bedrooms", level == 0 ? "upper" : "lower"));
    corridor_center0->layout.push_back(new_door(-1, 0));
    corridor_center0->layout.push_back(new_door(-1, 2));
    corridor_center0->accesspath = entr;
    corridors.push_back(corridor_center0);

    room *corridor_center2 = new room(f + df::coord(1, -1, 0), f + df::coord(1, 1, 0), stl_sprintf("main staircase - east %s bedrooms", level == 0 ? "upper" : "lower"));
    corridor_center2->layout.push_back(new_door(1, 0));
    corridor_center2->layout.push_back(new_door(1, 2));
    corridor_center2->accesspath = entr;
    corridors.push_back(corridor_center2);

    for (int16_t dirx = -1; dirx <= 1; dirx += 2)
    {
        room *prev_corx = dirx < 0 ? corridor_center0 : corridor_center2;
        int16_t ocx = f.x + dirx * 3;
        for (int16_t dx = 1; dx <= 3; dx++)
        {
            // segments of the big central horizontal corridor
            int16_t cx = f.x + dirx * (9 * dx - 4);
            room *cor_x = new room(df::coord(ocx, f.y - 1, f.z), df::coord(cx, f.y + 1, f.z), stl_sprintf("%s %s bedrooms - segment %d", dirx == -1 ? "west" : "east", level == 0 ? "upper" : "lower", dx));
            cor_x->accesspath.push_back(prev_corx);
            corridors.push_back(cor_x);
            prev_corx = cor_x;
            ocx = cx + dirx;

            for (int16_t diry = -1; diry <= 1; diry += 2)
            {
                room *prev_cory = cor_x;
                int16_t ocy = f.y + diry * 2;
                for (int16_t dy = 1; dy <= 6; dy++)
                {
                    int16_t cy = f.y + diry * 3 * dy;
                    room *cor_y = new room(df::coord(cx, ocy, f.z), df::coord(cx - dirx, cy, f.z), stl_sprintf("%s %s %s bedrooms - segment %d-%d", diry == -1 ? "north" : "south", dirx == -1 ? "west" : "east", level == 0 ? "upper" : "lower", dx, dy));
                    cor_y->accesspath.push_back(prev_cory);
                    corridors.push_back(cor_y);
                    prev_cory = cor_y;
                    ocy = cy + diry;

                    auto bedroom = [this, cx, diry, cor_y](room *r)
                    {
                        r->accesspath.push_back(cor_y);
                        r->layout.push_back(new_furniture("bed", r->min.x < cx ? 0 : 1, diry < 0 ? 1 : 0));
                        r->layout.back()->makeroom = true;
                        r->layout.push_back(new_furniture("cabinet", r->min.x < cx ? 0 : 1, diry < 0 ? 0 : 1));
                        r->layout.back()->ignore = true;
                        r->layout.push_back(new_furniture("chest", r->min.x < cx ? 1 : 0, diry < 0 ? 0 : 1));
                        r->layout.back()->ignore = true;
                        r->layout.push_back(new_door(r->min.x < cx ? 2 : -1, diry < 0 ? 1 : 0));
                        rooms.push_back(r);
                    };
                    bedroom(new room(room_type::bedroom, "", df::coord(cx - dirx * 4, cy, f.z), df::coord(cx - dirx * 3, cy + diry, f.z), stl_sprintf("%s %s %s bedrooms - room %d-%d A", diry == -1 ? "north" : "south", dirx == -1 ? "west" : "east", level == 0 ? "upper" : "lower", dx, dy)));
                    bedroom(new room(room_type::bedroom, "", df::coord(cx + dirx * 2, cy, f.z), df::coord(cx + dirx * 3, cy + diry, f.z), stl_sprintf("%s %s %s bedrooms - room %d-%d B", diry == -1 ? "north" : "south", dirx == -1 ? "west" : "east", level == 0 ? "upper" : "lower", dx, dy)));
                }
            }
        }

        // noble suites
        int16_t cx = f.x + dirx * (9 * 3 - 4 + 6);
        room *cor_x = new room(df::coord(ocx, f.y - 1, f.z), df::coord(cx, f.y + 1, f.z), stl_sprintf("%s %s bedrooms - noble room hallway", dirx == -1 ? "west" : "east", level == 0 ? "upper" : "lower"));
        room *cor_x2 = new room(df::coord(ocx - dirx, f.y, f.z), df::coord(f.x + dirx * 3, f.y, f.z));
        cor_x->accesspath.push_back(cor_x2);
        cor_x2->accesspath.push_back(dirx < 0 ? corridor_center0 : corridor_center2);
        corridors.push_back(cor_x);
        corridors.push_back(cor_x2);

        for (int16_t diry = -1; diry <= 1; diry += 2)
        {
            noblesuite++;

            room *r = new room(room_type::nobleroom, "office", df::coord(cx - 1, f.y + diry * 3, f.z), df::coord(cx + 1, f.y + diry * 5, f.z));
            r->noblesuite = noblesuite;
            r->accesspath.push_back(cor_x);
            r->layout.push_back(new_furniture("chair", 1, 1));
            r->layout.back()->makeroom = true;
            r->layout.push_back(new_furniture("table", 1 - dirx, 1));
            r->layout.push_back(new_furniture("chest", 1 + dirx, 0));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_furniture("cabinet", 1 + dirx, 2));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_door(1, 1 - 2 * diry));
            rooms.push_back(r);

            r = new room(room_type::nobleroom, "bedroom", df::coord(cx - 1, f.y + diry * 7, f.z), df::coord(cx + 1, f.y + diry * 9, f.z));
            r->noblesuite = noblesuite;
            r->layout.push_back(new_furniture("bed", 1, 1));
            r->layout.back()->makeroom = true;
            r->layout.push_back(new_furniture("armorstand", 1 - dirx, 0));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_furniture("weaponrack", 1 - dirx, 2));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_door(1, 1 - 2 * diry, true));
            r->accesspath.push_back(rooms.back());
            rooms.push_back(r);

            r = new room(room_type::nobleroom, "diningroom", df::coord(cx - 1, f.y + diry * 11, f.z), df::coord(cx + 1, f.y + diry * 13, f.z));
            r->noblesuite = noblesuite;
            r->layout.push_back(new_furniture("table", 1, 1));
            r->layout.back()->makeroom = true;
            r->layout.push_back(new_furniture("chair", 1 + dirx, 1));
            r->layout.push_back(new_furniture("cabinet", 1 - dirx, 0));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_furniture("chest", 1 - dirx, 2));
            r->layout.back()->ignore = true;
            r->layout.push_back(new_door(1, 1 - 2 * diry, true));
            r->accesspath.push_back(rooms.back());
            rooms.push_back(r);

            r = new room(room_type::nobleroom, "tomb", df::coord(cx - 1, f.y + diry * 15, f.z), df::coord(cx + 1, f.y + diry * 17, f.z));
            r->noblesuite = noblesuite;
            r->layout.push_back(new_furniture("coffin", 1, 1));
            r->layout.back()->makeroom = true;
            r->layout.push_back(new_door(1, 1 - 2 * diry, true));
            r->accesspath.push_back(rooms.back());
            rooms.push_back(r);
        }
    }
    return CR_OK;
}

static int16_t setup_outdoor_gathering_zones_counters[3];
static std::map<int16_t, std::set<df::coord2d>> setup_outdoor_gathering_zones_ground;

command_result Plan::setup_outdoor_gathering_zones(color_ostream &)
{
    setup_outdoor_gathering_zones_counters[0] = 0;
    setup_outdoor_gathering_zones_counters[1] = 0;
    setup_outdoor_gathering_zones_counters[2] = 0;
    setup_outdoor_gathering_zones_ground.clear();
    events.onupdate_register_once("df-ai plan setup_outdoor_gathering_zones", 10, [this](color_ostream & out) -> bool
            {
                int16_t & x = setup_outdoor_gathering_zones_counters[0];
                int16_t & y = setup_outdoor_gathering_zones_counters[1];
                int16_t & i = setup_outdoor_gathering_zones_counters[2];
                std::map<int16_t, std::set<df::coord2d>> & ground = setup_outdoor_gathering_zones_ground;
                if (i == 31 || x + i == world->map.x_count)
                {
                    for (auto g = ground.begin(); g != ground.end(); g++)
                    {
                        df::building_civzonest *bld = virtual_cast<df::building_civzonest>(Buildings::allocInstance(df::coord(x, y, g->first), building_type::Civzone, civzone_type::ActivityZone));
                        int16_t w = 31;
                        int16_t h = 31;
                        if (x + 31 > world->map.x_count)
                            w = world->map.x_count % 31;
                        if (y + 31 > world->map.y_count)
                            h = world->map.y_count % 31;
                        Buildings::setSize(bld, df::coord(w, h, 1));
                        delete[] bld->room.extents;
                        bld->room.extents = new uint8_t[w * h]();
                        bld->room.x = x;
                        bld->room.y = y;
                        bld->room.width = w;
                        bld->room.height = h;
                        for (int16_t dx = 0; dx < w; dx++)
                        {
                            for (int16_t dy = 0; dy < h; dy++)
                            {
                                bld->room.extents[dx + w * dy] = g->second.count(df::coord2d(dx, dy)) ? 1 : 0;
                            }
                        }
                        Buildings::constructAbstract(bld);
                        bld->is_room = true;

                        bld->zone_flags.bits.active = 1;
                        bld->zone_flags.bits.gather = 1;
                        bld->gather_flags.bits.pick_trees = 1;
                        bld->gather_flags.bits.pick_shrubs = 1;
                        bld->gather_flags.bits.gather_fallen = 1;
                    }

                    ground.clear();
                    i = 0;
                    x += 31;
                    if (x >= world->map.x_count)
                    {
                        x = 0;
                        y += 31;
                        if (y >= world->map.y_count)
                        {
                            ai->debug(out, "plan setup_outdoor_gathering_zones finished");
                            return true;
                        }
                    }
                    return false;
                }

                int16_t tx = x + i;
                for (int16_t ty = y; ty < y + 31 && ty < world->map.y_count; ty++)
                {
                    df::coord t = surface_tile_at(tx, ty, true);
                    if (!t.isValid())
                        continue;
                    ground[t.z].insert(df::coord2d(tx % 31, ty % 31));
                }
                i++;
                return false;
            });

    return CR_OK;
}

command_result Plan::setup_blueprint_caverns(color_ostream & out)
{
    df::coord wall;
    wall.clear();
    int16_t & z = cavern_max_level;
    if (z == -1)
    {
        z = world->map.z_count - 1;
    }
    df::coord target;
    for (; !wall.isValid() && z > 0; z--)
    {
        ai->debug(out, stl_sprintf("outpost: searching z-level %d", z));
        for (int16_t x = 0; !wall.isValid() && x < world->map.x_count; x++)
        {
            for (int16_t y = 0; !wall.isValid() && y < world->map.y_count; y++)
            {
                df::coord t(x, y, z);
                if (!map_tile_in_rock(t))
                    continue;
                // find a floor next to the wall
                target = spiral_search(t, 2, 2, [this](df::coord _t) -> bool
                        {
                            return map_tile_cavernfloor(_t);
                        });
                if (target.isValid())
                    wall = t;
            }
        }
    }
    if (!wall.isValid())
    {
        ai->debug(out, "outpost: could not find a cavern wall tile");
        return CR_FAILURE;
    }

    room *r = new room(room_type::outpost, "cavern", target, target);

    int16_t y_x = 0;
    if (wall.x - 1 > target.x)
    {
        room *cor = new room(df::coord(wall.x + 1, wall.y, wall.z), df::coord(target.x - 1, wall.y, wall.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
        y_x = 1;
    }
    else if (target.x - 1 > wall.x)
    {
        room *cor = new room(df::coord(wall.x - 1, wall.y, wall.z), df::coord(target.x + 1, wall.y, wall.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
        y_x = -1;
    }

    if (wall.y - 1 > target.y)
    {
        room *cor = new room(df::coord(target.x + y_x, wall.y + 1, wall.z), df::coord(target.x + y_x, target.y, wall.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
    }
    else if (target.y - 1 > wall.y)
    {
        room *cor = new room(df::coord(target.x + y_x, wall.y - 1, wall.z), df::coord(target.x + y_x, target.y, wall.z));
        corridors.push_back(cor);
        r->accesspath.push_back(cor);
    }

    std::vector<room *> up = find_corridor_tosurface(out, wall);
    r->accesspath.push_back(up.at(0));

    ai->debug(out, stl_sprintf("outpost: wall (%d, %d, %d)", wall.x, wall.y, wall.z));
    ai->debug(out, stl_sprintf("outpost: target (%d, %d, %d)", target.x, target.y, target.z));
    ai->debug(out, stl_sprintf("outpost: up (%d, %d, %d)", up.back()->max.x, up.back()->max.y, up.back()->max.z));

    rooms.push_back(r);

    return CR_OK;
}

// create a new Corridor from origin to surface, through rock
// may create multiple chunks to avoid obstacles, all parts are added to corridors
// returns an array of Corridors, 1st = origin, last = surface
std::vector<room *> Plan::find_corridor_tosurface(color_ostream & out, df::coord origin)
{
    std::vector<room *> cors;
    for (;;)
    {
        room *cor = new room(origin, origin);
        if (!cors.empty())
        {
            cors.back()->accesspath.push_back(cor);
        }
        cors.push_back(cor);

        while (map_tile_in_rock(cor->max) && !map_tile_intersects_room(cor->max + df::coord(0, 0, 1)) && Maps::isValidTilePos(cor->max.x, cor->max.y, cor->max.z + 1))
        {
            cor->max.z++;
        }

        df::tiletype tt = *Maps::getTileType(cor->max);
        df::tiletype_shape_basic sb = ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, tt));
        df::tiletype_material tm = ENUM_ATTR(tiletype, material, tt);
        df::tile_designation td = *Maps::getTileDesignation(cor->max);
        if ((sb == tiletype_shape_basic::Ramp ||
                    sb == tiletype_shape_basic::Floor) &&
                tm != tiletype_material::TREE &&
                td.bits.flow_size == 0 &&
                !td.bits.hidden)
        {
            break;
        }

        df::coord out2 = spiral_search(cor->max, [this](df::coord t) -> bool
                {
                    while (map_tile_in_rock(t))
                    {
                        t.z++;
                    }

                    df::tiletype *tt = Maps::getTileType(t);
                    if (!tt)
                        return false;

                    df::tiletype_shape_basic sb = ENUM_ATTR(tiletype_shape, basic_shape, ENUM_ATTR(tiletype, shape, *tt));
                    df::tile_designation td = *Maps::getTileDesignation(t);

                    return (sb == tiletype_shape_basic::Ramp || sb == tiletype_shape_basic::Floor) &&
                            ENUM_ATTR(tiletype, material, *tt) != tiletype_material::TREE &&
                            td.bits.flow_size == 0 &&
                            !td.bits.hidden &&
                            !map_tile_intersects_room(t);
                });

        if (!out2.isValid())
        {
            ai->debug(out, stl_sprintf("[ERROR] could not find corridor to surface (%d, %d, %d)", cor->max.x, cor->max.y, cor->max.z));
            break;
        }

        if (Maps::getTileDesignation(cor->max)->bits.flow_size > 0)
        {
            // damp stone located
            cor->max.z--;
            out2.z--;
        }
        cor->max.z--;
        out2.z--;

        int16_t y_x = cor->max.x;
        if (cor->max.x - 1 > out2.x)
        {
            cor = new room(df::coord(out2.x - 1, out2.y, out2.z), df::coord(cor->max.x + 1, out2.y, out2.z));
            cors.back()->accesspath.push_back(cor);
            cors.push_back(cor);
            y_x++;
        }
        else if (out2.x - 1 > cor->max.x)
        {
            cor = new room(df::coord(out2.x + 1, out2.y, out2.z), df::coord(cor->max.x - 1, out2.y, out2.z));
            cors.back()->accesspath.push_back(cor);
            cors.push_back(cor);
            y_x--;
        }

        if (cor->max.y - 1 > out2.y)
        {
            cor = new room(df::coord(y_x, out2.y - 1, out2.z), df::coord(y_x, cor->max.y, out2.z));
            cors.back()->accesspath.push_back(cor);
            cors.push_back(cor);
        }
        else if (out2.y - 1 > cor->max.y)
        {
            cor = new room(df::coord(y_x, out2.y + 1, out2.z), df::coord(y_x, cor->max.y, out2.z));
            cors.back()->accesspath.push_back(cor);
            cors.push_back(cor);
        }

        if (origin == out2)
        {
            ai->debug(out, stl_sprintf("[ERROR] find_corridor_tosurface: loop: %d, %d, %d", origin.x, origin.y, origin.z));
            break;
        }
        ai->debug(out, stl_sprintf("find_corridor_tosurface: %d, %d, %d -> %d, %d, %d", origin.x, origin.y, origin.z, out2.x, out2.y, out2.z));

        origin = out2;
    }
    corridors.insert(corridors.end(), cors.begin(), cors.end());
    return cors;
}
