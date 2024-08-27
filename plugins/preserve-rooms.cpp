#include "Core.h"
#include "Debug.h"
#include "LuaTools.h"
#include "PluginManager.h"

#include "modules/Buildings.h"
#include "modules/Gui.h"
#include "modules/Translation.h"
#include "modules/Units.h"
#include "modules/World.h"

#include "df/building_civzonest.h"
#include "df/historical_figure.h"
#include "df/unit.h"
#include "df/world.h"

#include <string>
#include <unordered_map>
#include <vector>

using std::pair;
using std::string;
using std::unordered_map;
using std::vector;

using namespace DFHack;

DFHACK_PLUGIN("preserve-rooms");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(world);

namespace DFHack {
    DBG_DECLARE(persistent_per_save_example, control, DebugCategory::LINFO);
    DBG_DECLARE(persistent_per_save_example, cycle, DebugCategory::LINFO);
}

static const string CONFIG_KEY = string(plugin_name) + "/config";
static PersistentDataItem config;

// zone id -> hfids (includes spouses)
static vector<pair<int32_t, vector<int32_t>>> last_known_assignments_bedroom;
static vector<pair<int32_t, vector<int32_t>>> last_known_assignments_office;
static vector<pair<int32_t, vector<int32_t>>> last_known_assignments_dining;
static vector<pair<int32_t, vector<int32_t>>> last_known_assignments_tomb;
// hfid -> zone ids
static unordered_map<int32_t, vector<int32_t>> pending_reassignment;
// zone id -> hfids
static unordered_map<int32_t, vector<int32_t>> reserved_zones;

// zone id -> noble/administrative position code
static unordered_map<int32_t, string> noble_zones;

// as a "system" plugin, we do not persist plugin enabled state, just feature enabled state
enum ConfigValues {
    CONFIG_TRACK_MISSIONS = 0,
    CONFIG_TRACK_ROLES = 1,
};

static const int32_t CYCLE_TICKS = 109;
static int32_t cycle_timestamp = 0;  // world->frame_counter at last cycle

static command_result do_command(color_ostream &out, vector<string> &parameters);
static void do_cycle(color_ostream &out);

DFhackCExport command_result plugin_init(color_ostream &out, std::vector <PluginCommand> &commands) {
    DEBUG(control,out).print("initializing %s\n", plugin_name);
    commands.push_back(PluginCommand(
        plugin_name,
        "Manage room assignments for off-map units and noble roles.",
        do_command));
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    is_enabled = enable;
    DEBUG(control, out).print("now %s\n", is_enabled ? "enabled" : "disabled");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    DEBUG(control,out).print("shutting down %s\n", plugin_name);
    return CR_OK;
}

static void clear_track_missions_state() {
    last_known_assignments_bedroom.clear();
    last_known_assignments_office.clear();
    last_known_assignments_dining.clear();
    last_known_assignments_tomb.clear();
    pending_reassignment.clear();
    reserved_zones.clear();
}

static void clear_track_roles_state() {
    noble_zones.clear();
}

DFhackCExport command_result plugin_load_site_data(color_ostream &out) {
    cycle_timestamp = 0;
    config = World::GetPersistentSiteData(CONFIG_KEY);

    if (!config.isValid()) {
        DEBUG(control,out).print("no config found in this save; initializing\n");
        config = World::AddPersistentSiteData(CONFIG_KEY);
        config.set_bool(CONFIG_TRACK_MISSIONS, false);
        config.set_bool(CONFIG_TRACK_ROLES, true);
    }

    clear_track_missions_state();
    clear_track_roles_state();

    // TODO: restore vectors/maps from serialized state

    return CR_OK;
}

DFhackCExport command_result plugin_save_site_data (color_ostream &out) {
    // TODO: serialize vectors/maps

    return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event) {
    if (event == DFHack::SC_WORLD_UNLOADED) {
        if (is_enabled) {
            DEBUG(control,out).print("world unloaded; disabling %s\n",
                                    plugin_name);
            is_enabled = false;
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    if (!Core::getInstance().isMapLoaded() || !World::isFortressMode())
        return CR_OK;
    if (world->frame_counter - cycle_timestamp >= CYCLE_TICKS)
        do_cycle(out);
    return CR_OK;
}

static command_result do_command(color_ostream &out, vector<string> &parameters) {
    CoreSuspender suspend;

    if (!World::isFortressMode() || !Core::getInstance().isMapLoaded()) {
        out.printerr("Cannot run %s without a loaded fort.\n", plugin_name);
        return CR_FAILURE;
    }

    bool show_help = false;
    if (!Lua::CallLuaModuleFunction(out, "plugins.preserve-rooms", "parse_commandline", std::make_tuple(parameters),
            1, [&](lua_State *L) {
                show_help = !lua_toboolean(L, -1);
            })) {
        return CR_FAILURE;
    }

    return show_help ? CR_WRONG_USAGE : CR_OK;
}

/////////////////////////////////////////////////////
// cycle logic
//

static bool is_noble_zone(int32_t zone_id, const string & code) {
    auto it = noble_zones.find(zone_id);
    return it != noble_zones.end() && it->second == code;
}

static void assign_nobles(color_ostream &out) {
    for (auto &[zone_id, code] : noble_zones) {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone)
            continue;
        vector<df::unit *> units;
        Units::getUnitsByNobleRole(units, code);
        // if zone is already assigned to a proper unit, skip
        if (linear_index(units, zone->assigned_unit) >= 0)
            continue;
        // assign to a relevant noble that does not already have a registered zone of this type assigned
        for (auto unit : units) {
            if (!Units::isCitizen(unit, true) && !Units::isResident(unit, true))
                continue;
            bool found = false;
            for (auto owned_zone : unit->owned_buildings) {
                if (owned_zone->type == zone->type && is_noble_zone(owned_zone->id, code)) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            Buildings::setOwner(zone, unit);
            INFO(cycle,out).print("assigning %s to a %s-associated %s\n",
                Translation::TranslateName(&unit->name, false).c_str(), code.c_str(),
                ENUM_KEY_STR(civzone_type, zone->type).c_str());
            break;
        }
    }
}

static void clear_reservation(color_ostream &out, int32_t zone_id, df::building_civzonest * zone = NULL) {
    auto it = reserved_zones.find(zone_id);
    if (it == reserved_zones.end())
        return;
    for (auto hfid : it->second) {
        auto pending_it = pending_reassignment.find(hfid);
        if (pending_it != pending_reassignment.end()) {
            auto & zone_ids = pending_it->second;
            vector_erase_at(zone_ids, linear_index(zone_ids, zone_id));
            if (zone_ids.empty())
                pending_reassignment.erase(pending_it);
        }
    }
    reserved_zones.erase(zone_id);
    if (!zone)
        zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
    if (zone)
        zone->spec_sub_flag.bits.active = true;
}

static void scan_assignments(color_ostream &out,
    vector<pair<int32_t, vector<int32_t>>> & last_known,
    const vector<df::building_civzonest *> & vec,
    bool exclude_spouse = false)
{
    // auto it = last_known.begin();

    vector<pair<int32_t, vector<int32_t>>> assignments;
    // for (auto zone : vec) {

    // }

    last_known = assignments;
}

static void do_cycle(color_ostream &out) {
    cycle_timestamp = world->frame_counter;

    DEBUG(cycle,out).print("running %s cycle\n", plugin_name);

    assign_nobles(out);

    scan_assignments(out, last_known_assignments_bedroom, world->buildings.other.ZONE_BEDROOM);
    scan_assignments(out, last_known_assignments_office, world->buildings.other.ZONE_OFFICE);
    scan_assignments(out, last_known_assignments_dining, world->buildings.other.ZONE_DINING_HALL);
    scan_assignments(out, last_known_assignments_tomb, world->buildings.other.ZONE_TOMB, true);
}

/////////////////////////////////////////////////////
// Lua API
//

static void preserve_rooms_cycle(color_ostream &out) {
    DEBUG(control,out).print("entering preserve_rooms_cycle\n");
    do_cycle(out);
}

static bool preserve_rooms_setFeature(color_ostream &out, bool enabled, string feature) {
    DEBUG(control,out).print("entering preserve_rooms_setFeature (enabled=%d, feature=%s)\n",
        enabled, feature.c_str());
    if (feature == "track-missions") {
        config.set_bool(CONFIG_TRACK_MISSIONS, enabled);
        if (is_enabled && enabled)
            do_cycle(out);
    } else if (feature == "track-roles") {
        config.set_bool(CONFIG_TRACK_ROLES, enabled);
    } else {
        return false;
    }

    return true;
}

static bool preserve_rooms_resetFeatureState(color_ostream &out, string feature) {
    DEBUG(control,out).print("entering preserve_rooms_resetFeatureState (feature=%s)\n", feature.c_str());
    if (feature == "track-missions") {
        vector<int32_t> zone_ids;
        std::transform(reserved_zones.begin(), reserved_zones.end(), std::back_inserter(zone_ids), [](auto & elem){ return elem.first; });
        for (auto zone_id : zone_ids)
            clear_reservation(out, zone_id);
        clear_track_missions_state();
    } else if (feature == "track-roles") {
        clear_track_roles_state();
    } else {
        return false;
    }

    return true;
}

static void preserve_rooms_assignToRole(color_ostream &out, string code) {
    auto zone = Gui::getSelectedCivZone(out, true);
    if (!zone)
        return;
    noble_zones[zone->id] = code;
    do_cycle(out);
}

static string preserve_rooms_getRoleAssignment(color_ostream &out) {
    auto zone = Gui::getSelectedCivZone(out, true);
    if (!zone)
        return "";
    auto it = noble_zones.find(zone->id);
    if (it == noble_zones.end())
        return "";
    return it->second;
}

static bool preserve_rooms_isReserved(color_ostream &out) {
    auto zone = Gui::getSelectedCivZone(out, true);
    if (!zone)
        return false;
    auto it = reserved_zones.find(zone->id);
    return it != reserved_zones.end() && it->second.size() > 0;
}

static string preserve_rooms_getReservationName(color_ostream &out) {
    auto zone = Gui::getSelectedCivZone(out, true);
    if (!zone)
        return "";
    auto it = reserved_zones.find(zone->id);
    if (it != reserved_zones.end() && it->second.size() > 0) {
        if (auto hf = df::historical_figure::find(it->second.front())) {
            return Translation::TranslateName(&hf->name, false);
        }
    }
    return "";
}

static bool preserve_rooms_clearReservation(color_ostream &out) {
    auto zone = Gui::getSelectedCivZone(out, true);
    if (!zone)
        return false;
    clear_reservation(out, zone->id, zone);
    return true;
}

static int preserve_rooms_getState(lua_State *L) {
    color_ostream *out = Lua::GetOutput(L);
    if (!out)
        out = &Core::getInstance().getConsole();
    DEBUG(control,*out).print("entering preserve_rooms_getState\n");

    unordered_map<string, bool> features;
    features.emplace("track-missions", config.get_bool(CONFIG_TRACK_MISSIONS));
    features.emplace("track-roles", config.get_bool(CONFIG_TRACK_ROLES));
    Lua::Push(L, features);

    return 1;
}

DFHACK_PLUGIN_LUA_FUNCTIONS{
    DFHACK_LUA_FUNCTION(preserve_rooms_cycle),
    DFHACK_LUA_FUNCTION(preserve_rooms_setFeature),
    DFHACK_LUA_FUNCTION(preserve_rooms_resetFeatureState),
    DFHACK_LUA_FUNCTION(preserve_rooms_assignToRole),
    DFHACK_LUA_FUNCTION(preserve_rooms_getRoleAssignment),
    DFHACK_LUA_FUNCTION(preserve_rooms_isReserved),
    DFHACK_LUA_FUNCTION(preserve_rooms_getReservationName),
    DFHACK_LUA_FUNCTION(preserve_rooms_clearReservation),
    DFHACK_LUA_END};

DFHACK_PLUGIN_LUA_COMMANDS{
    DFHACK_LUA_COMMAND(preserve_rooms_getState),
    DFHACK_LUA_END};
