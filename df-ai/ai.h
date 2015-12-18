#pragma once

#include "Core.h"
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>

#include "DataDefs.h"

#include <random>
#include <ctime>

#include "df/report.h"

using namespace DFHack;
using namespace df::enums;

extern std::vector<std::string> *plugin_globals;

class Population;
class Plan;
class Stocks;
class Camera;
class Embark;
class AI;

class Population
{
    AI *ai;
    std::set<int32_t> citizens;
    std::map<int32_t, int32_t> military;
    std::set<int32_t> idlers;
    std::set<int32_t> pets;
    int update_tick;

public:
    Population(color_ostream & out, AI *parent);
    ~Population();

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);

private:
    void update_citizenlist(color_ostream & out);
    void update_nobles(color_ostream & out);
    void update_jobs(color_ostream & out);
    void update_military(color_ostream & out);
    void update_pets(color_ostream & out);
    void update_deads(color_ostream & out);
    void update_caged(color_ostream & out);
    void autolabors_workers(color_ostream & out);
    void autolabors_jobs(color_ostream & out);
    void autolabors_labors(color_ostream & out);
    void autolabors_commit(color_ostream & out);
};

class Plan
{
    AI *ai;

public:
    Plan(color_ostream & out, AI *parent);
    ~Plan();

    enum room_status
    {
        plan,
        finished,
    };
    enum room_type
    {
        infirmary,
        barracks,
    };
    struct room
    {
        room_type type;
        room_status status;
        union T_info
        {
            struct
            {
                int32_t squad_id;
            } barracks;
        };
        T_info info;
    };

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);

    template<class F>
    room *find_room(room_type type, F filter)
    {
        // TODO
        return nullptr;
    }
    room *find_room(room_type type)
    {
        return find_room(type, [](room *r) -> bool { return true; });
    }

    void new_citizen(color_ostream & out, int32_t id);
    void del_citizen(color_ostream & out, int32_t id);
    void attribute_noblerooms(color_ostream & out, std::set<int32_t> & ids);
    void new_soldier(color_ostream & out, int32_t id);
    void del_soldier(color_ostream & out, int32_t id);
};

class Stocks
{
    AI *ai;

public:
    Stocks(color_ostream & out, AI *parent);
    ~Stocks();

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);
};

class Camera
{
    AI *ai;
    int32_t following;
    int32_t following_prev[3];
    int following_index;
    int update_after_ticks;

public:
    Camera(color_ostream & out, AI *parent);
    ~Camera();

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);

    void start_recording(color_ostream & out);
    bool followed_previously(int32_t id);
};

class Embark
{
    AI *ai;
    bool embarking;
    std::string world_name;
    std::time_t timeout;

public:
    Embark(color_ostream & out, AI *parent);
    ~Embark();

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);
};

class AI
{
protected:
    std::mt19937 rng;
    Population pop;
    friend class Population;
    Plan plan;
    friend class Plan;
    Stocks stocks;
    friend class Stocks;
    Camera camera;
    friend class Camera;
    Embark embark;
    friend class Embark;
    std::time_t unpause_delay;

public:
    AI(color_ostream & out);
    ~AI();

    command_result status(color_ostream & out);
    command_result statechange(color_ostream & out, state_change_event event);
    command_result update(color_ostream & out);

    void debug(color_ostream & out, const std::string & str);
    void unpause(color_ostream & out);
    void check_unpause(color_ostream & out, state_change_event event);
    void handle_pause_event(color_ostream & out, std::vector<df::report *>::reverse_iterator ann, std::vector<df::report *>::reverse_iterator end);
};

// vim: et:sw=4:ts=4