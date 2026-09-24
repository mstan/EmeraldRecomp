// emerald_touch_field.cpp — overworld touch: draw a path, tap to walk, tap to
// interact, long-press for Start (routed in emerald_touch.cpp).
//
// The passability model mirrors the game's own step rules
// (event_object_movement.c GetCollisionAtCoords + field_player_avatar.c
// CheckForObjectEventCollision) read from live guest state: backup map grid
// collision/elevation, metatile behaviours from the current tilesets,
// connection borders, directional walls, ledges, surf landing and objects.
// Every executed step is checked against what the guest actually did; a
// mismatch is logged to the always-on step ring and the path is replanned.

#include "emerald_touch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "host_overlay.h"

namespace emerald::touch {

namespace k = gbarecomp;
using namespace guest;

namespace {

enum Dir : int { kNone = 0, kSouth = 1, kNorth = 2, kWest = 3, kEast = 4 };
const int kDx[5] = {0, 0, 0, -1, 1};
const int kDy[5] = {0, 1, -1, 0, 0};
std::uint16_t dir_key(int d) {
    switch (d) {
        case kSouth: return k::kGbaKeyDown;
        case kNorth: return k::kGbaKeyUp;
        case kWest: return k::kGbaKeyLeft;
        case kEast: return k::kGbaKeyRight;
    }
    return 0;
}
int opposite(int d) {
    switch (d) {
        case kSouth: return kNorth;
        case kNorth: return kSouth;
        case kWest: return kEast;
        case kEast: return kWest;
    }
    return kNone;
}

constexpr std::uint8_t kFlagSurfing = 1 << 3, kFlagMachBike = 1 << 1,
                       kFlagAcroBike = 1 << 2, kFlagForcedMove = 1 << 6;
constexpr std::uint32_t kSaveFlagsOffset = 0x1270, kFlagSysBDash = 0x8C0;
constexpr std::uint32_t kConnectionFlags = 0x02037340;   // sMapConnectionFlags

struct Tile {
    int x = 0, y = 0;
    bool operator==(const Tile& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Tile& o) const { return !(*this == o); }
};

// ── Live passability model ──────────────────────────────────────────────
struct Model {
    const Mem* m = nullptr;
    int w = 0, h = 0;
    std::uint32_t map = 0, layout = 0, primary_attrs = 0, secondary_attrs = 0, border = 0;
    std::uint8_t conn = 0;
    int player_object = 0;

    bool load(const Mem& mem) {
        m = &mem;
        w = static_cast<int>(mem.u32(addr::gBackupMapLayout + 0));
        h = static_cast<int>(mem.u32(addr::gBackupMapLayout + 4));
        map = mem.u32(addr::gBackupMapLayout + 8);
        layout = mem.u32(addr::gMapHeader + 0);
        if (w <= 0 || h <= 0 || w > 512 || h > 512 || !mem.ptr(map, 2) || !layout) return false;
        border = mem.u32(layout + 0x08);
        const std::uint32_t primary = mem.u32(layout + 0x10);
        const std::uint32_t secondary = mem.u32(layout + 0x14);
        primary_attrs = primary ? mem.u32(primary + 0x10) : 0;
        secondary_attrs = secondary ? mem.u32(secondary + 0x10) : 0;
        conn = mem.u8(kConnectionFlags);
        player_object = mem.u8(addr::gPlayerAvatar + 5);
        return true;
    }
    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    std::uint16_t block(int x, int y) const {
        if (in_bounds(x, y)) return m->u16(map + 2u * (x + w * y));
        // GetBorderBlockAt: border metatile | MAPGRID_IMPASSABLE.
        return static_cast<std::uint16_t>(
            m->u16(border + 2u * (((x + 1) & 1) + (((y + 1) & 1) << 1))) | 0x0C00);
    }
    bool undefined(int x, int y) const { return block(x, y) == 0x03FF; }
    int collision(int x, int y) const {
        const std::uint16_t b = block(x, y);
        return b == 0x03FF ? 1 : (b >> 10) & 3;
    }
    int elevation(int x, int y) const {
        const std::uint16_t b = block(x, y);
        return b == 0x03FF ? 0 : b >> 12;
    }
    std::uint8_t behavior(int x, int y) const {
        std::uint16_t id = block(x, y) & 0x03FF;
        if (id == 0x03FF) id = m->u16(border + 2u * (((x + 1) & 1) + (((y + 1) & 1) << 1))) & 0x3FF;
        if (id < 512) return primary_attrs ? m->u16(primary_attrs + 2u * id) & 0xFF : 0xFF;
        if (id < 1024) return secondary_attrs ? m->u16(secondary_attrs + 2u * (id - 512)) & 0xFF : 0xFF;
        return 0xFF;
    }
    // GetMapBorderIdAt: -1 = CONNECTION_INVALID.
    bool border_invalid(int x, int y) const {
        if (undefined(x, y)) return true;
        if (x >= w - 8) return !(conn & 8);
        if (x < 7) return !(conn & 4);
        if (y >= h - 7) return !(conn & 1);
        if (y < 7) return !(conn & 2);
        return false;
    }
    bool object_at(int x, int y, int elev, int ignore = -1) const {
        for (int i = 0; i < 16; ++i) {
            if (i == player_object || i == ignore) continue;
            const std::uint32_t o = addr::gObjectEvents + i * off::kObjectEventSize;
            if (!(m->u8(o) & 1)) continue;
            const int cx = m->s16(o + 0x10), cy = m->s16(o + 0x12);
            const int px = m->s16(o + 0x14), py = m->s16(o + 0x16);
            if ((cx == x && cy == y) || (px == x && py == y)) {
                const int oe = m->u8(o + 0x0B) & 0x0F;
                if (elev == 0 || oe == 0 || elev == oe) return true;
            }
        }
        return false;
    }
    int object_id_at(int x, int y) const {
        for (int i = 0; i < 16; ++i) {
            if (i == player_object) continue;
            const std::uint32_t o = addr::gObjectEvents + i * off::kObjectEventSize;
            if (!(m->u8(o) & 1)) continue;
            if (m->s16(o + 0x10) == x && m->s16(o + 0x12) == y) return i;
        }
        return -1;
    }
};

bool north_blocked(std::uint8_t b) { return b == 0x32 || b == 0x34 || b == 0x35 || b == 0xC0; }
bool south_blocked(std::uint8_t b) { return b == 0x33 || b == 0x36 || b == 0x37 || b == 0xC0; }
bool east_blocked(std::uint8_t b) { return b == 0x30 || b == 0x34 || b == 0x36 || b == 0xC1 || b == 0xBE; }
bool west_blocked(std::uint8_t b) { return b == 0x31 || b == 0x35 || b == 0x37 || b == 0xC1 || b == 0xBE; }
// gDirectionBlockedMetatileFuncs / gOppositeDirectionBlockedMetatileFuncs.
bool enter_blocked(int d, std::uint8_t b) {
    switch (d) {
        case kSouth: return north_blocked(b);
        case kNorth: return south_blocked(b);
        case kWest: return east_blocked(b);
        case kEast: return west_blocked(b);
    }
    return false;
}
bool leave_blocked(int d, std::uint8_t b) {
    switch (d) {
        case kSouth: return south_blocked(b);
        case kNorth: return north_blocked(b);
        case kWest: return west_blocked(b);
        case kEast: return east_blocked(b);
    }
    return false;
}
int ledge_dir(std::uint8_t b) {
    switch (b) {
        case 0x38: return kEast;
        case 0x39: return kWest;
        case 0x3A: return kNorth;
        case 0x3B: return kSouth;
    }
    return kNone;
}
// Tiles that take control away (ice, slides, currents, bike terrain): the
// planner routes around them; the player can still draw across them with
// the pad.
bool forced_tile(std::uint8_t b) {
    return b == 0x20 || b == 0x26 || b == 0x27 || (b >= 0x40 && b <= 0x57) ||
           (b >= 0xD0 && b <= 0xD6) || b == 0x13 || b == 0x66;
}
bool door_tile(std::uint8_t b) {
    return b == 0x60 || b == 0x69 || b == 0x6C || b == 0x8B || b == 0x8C || b == 0x8D ||
           b == 0xEA;
}
int arrow_warp_dir(std::uint8_t b) {
    switch (b) {
        case 0x62: return kEast;
        case 0x63: return kWest;
        case 0x64: return kNorth;
        case 0x65: case 0x6D: return kSouth;
    }
    return kNone;
}

struct Move {
    Tile to;
    int dir = kNone;
    bool jump = false;
    int elevation = 0;
};

// One step from `from` in direction d, mirroring the game's collision rules.
bool try_step(const Model& md, Tile from, int d, int elev, bool surfing, Move* out) {
    const int x = from.x + kDx[d], y = from.y + kDy[d];
    if (!md.in_bounds(x, y)) return false;
    const std::uint8_t here = md.behavior(from.x, from.y);
    const std::uint8_t there = md.behavior(x, y);
    // Ledge: the entered tile jumps the player one further tile.
    if (!surfing && ledge_dir(there) == d) {
        const int lx = x + kDx[d], ly = y + kDy[d];
        if (!md.in_bounds(lx, ly) || md.collision(lx, ly) || md.object_at(lx, ly, elev))
            return false;
        *out = {{lx, ly}, d, true, elev};
        return true;
    }
    if (md.collision(x, y) || md.border_invalid(x, y) || leave_blocked(d, here) ||
        enter_blocked(d, there))
        return false;
    if (forced_tile(there)) return false;
    int new_elev = elev;
    const int te = md.elevation(x, y);
    if (elev != 0 && te != 0 && te != 15 && te != elev) {
        // Elevation mismatch: only surf landing (CanStopSurfing) is allowed.
        if (!(surfing && te == 3 && !md.object_at(x, y, 3))) return false;
        new_elev = 3;
    }
    if (md.object_at(x, y, elev)) return false;
    if (te != 0 && te != 15) new_elev = te;
    *out = {{x, y}, d, false, new_elev};
    return true;
}

// A* from `start` to any tile in `goals`. `bias` tiles (the drawn stroke)
// are cheaper so the route follows the player's line where possible.
std::vector<Move> plan(const Model& md, Tile start, int elev, bool surfing,
                       const std::vector<Tile>& goals,
                       const std::vector<Tile>& bias = {}) {
    std::vector<Move> none;
    if (goals.empty()) return none;
    auto key = [](int x, int y) { return (static_cast<std::uint32_t>(x) & 0xFFFF) |
                                         (static_cast<std::uint32_t>(y) << 16); };
    std::unordered_map<std::uint32_t, int> bias_set;
    for (const Tile& t : bias) bias_set[key(t.x, t.y)] = 1;
    auto heuristic = [&](int x, int y) {
        int best = 1 << 30;
        for (const Tile& g : goals) best = std::min(best, std::abs(g.x - x) + std::abs(g.y - y));
        return best * 2;
    };
    struct Node { int f, g; int x, y; };
    auto cmp = [](const Node& a, const Node& b) { return a.f > b.f; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
    struct Rec { std::uint32_t parent; Move move; int g; int elev; };
    std::unordered_map<std::uint32_t, Rec> seen;
    seen[key(start.x, start.y)] = {0xFFFFFFFFu, {start, kNone, false, elev}, 0, elev};
    open.push({heuristic(start.x, start.y), 0, start.x, start.y});
    int expansions = 0;
    while (!open.empty() && expansions < 6000) {
        const Node n = open.top();
        open.pop();
        const std::uint32_t nk = key(n.x, n.y);
        const Rec rec = seen[nk];
        if (n.g > rec.g) continue;
        ++expansions;
        for (const Tile& g : goals) {
            if (g.x != n.x || g.y != n.y) continue;
            std::vector<Move> path;
            for (std::uint32_t c = nk; seen[c].parent != 0xFFFFFFFFu; c = seen[c].parent)
                path.push_back(seen[c].move);
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (int d = 1; d <= 4; ++d) {
            Move mv;
            if (!try_step(md, {n.x, n.y}, d, rec.elev, surfing, &mv)) continue;
            const std::uint32_t ck = key(mv.to.x, mv.to.y);
            const int cost = (bias_set.count(ck) ? 2 : 5) * (mv.jump ? 2 : 1);
            const int g = n.g + cost;
            auto it = seen.find(ck);
            if (it != seen.end() && it->second.g <= g) continue;
            seen[ck] = {nk, mv, g, mv.elevation};
            open.push({g + heuristic(mv.to.x, mv.to.y), g, mv.to.x, mv.to.y});
        }
    }
    return none;
}

// ── Camera mapping ───────────────────────────────────────────────────────
struct Camera {
    int pos_x = 0, pos_y = 0, cam_x = 0, cam_y = 0, hpan = 0, vpan = 0;
    bool load(const Mem& m) {
        const std::uint32_t sb1 = m.u32(addr::gSaveBlock1Ptr);
        if (!m.ptr(sb1, 8)) return false;
        pos_x = m.s16(sb1 + 0);
        pos_y = m.s16(sb1 + 2);
        cam_x = static_cast<std::int32_t>(m.u32(addr::gFieldCamera + 16));
        cam_y = static_cast<std::int32_t>(m.u32(addr::gFieldCamera + 20));
        hpan = m.s16(addr::sHorizontalCameraPan);
        vpan = m.s16(addr::sVerticalCameraPan);
        return true;
    }
    int adj(int c) const { return c > 0 ? 16 : c < 0 ? -16 : 0; }
    // Native screen pixel of a map tile's top-left (SetSpritePosToMapCoords
    // with the sprite coordinate offset folded in).
    float tile_left(int x) const { return (x - pos_x) * 16.0f - cam_x + adj(cam_x) - hpan; }
    float tile_top(int y) const { return (y - pos_y) * 16.0f - cam_y + adj(cam_y) - vpan - 8.0f; }
    Tile tile_at(float nx, float ny) const {
        return {pos_x + static_cast<int>(std::floor((nx + cam_x - adj(cam_x) + hpan) / 16.0f)),
                pos_y + static_cast<int>(std::floor((ny + cam_y - adj(cam_y) + vpan + 8.0f) / 16.0f))};
    }
};

struct PlayerInfo {
    Tile pos, prev;
    int facing = kSouth, elevation = 0, transition = 0;
    std::uint8_t flags = 0;
    bool ok = false;
};
PlayerInfo read_player(const Mem& m) {
    PlayerInfo p;
    const int id = m.u8(addr::gPlayerAvatar + 5);
    if (id >= 16) return p;
    const std::uint32_t o = addr::gObjectEvents + id * off::kObjectEventSize;
    p.pos = {m.s16(o + 0x10), m.s16(o + 0x12)};
    p.prev = {m.s16(o + 0x14), m.s16(o + 0x16)};
    p.facing = m.u8(o + 0x18) & 0x0F;
    p.elevation = m.u8(o + 0x0B) & 0x0F;
    p.transition = m.u8(addr::gPlayerAvatar + 3);
    p.flags = m.u8(addr::gPlayerAvatar + 0);
    p.ok = true;
    return p;
}

bool running_allowed(const Mem& m) {
    const std::uint32_t sb1 = m.u32(addr::gSaveBlock1Ptr);
    const bool shoes = (m.u8(sb1 + kSaveFlagsOffset + kFlagSysBDash / 8) >> (kFlagSysBDash % 8)) & 1;
    const bool map_allows = (m.u8(addr::gMapHeader + 0x1A) >> 2) & 1;
    return shoes && map_allows;
}

std::pair<int, int> map_id(const Mem& m) {
    const std::uint32_t sb1 = m.u32(addr::gSaveBlock1Ptr);
    return {m.u8(sb1 + 4), m.u8(sb1 + 5)};
}

// ── Executor state ───────────────────────────────────────────────────────
enum class Goal { Walk, Interact, HoldInto };

struct Exec {
    bool active = false;
    std::vector<Move> path;
    std::size_t next = 0;           // index of the next move to perform
    Goal goal = Goal::Walk;
    int goal_dir = kNone;
    Tile target;                     // interaction / warp target tile
    Tile destination;                // final standing tile
    std::vector<Tile> stroke;        // drawn tiles (bias + replans)
    std::pair<int, int> map{-1, -1};
    Tile last_pos;
    int still_frames = 0, replans = 0, finish_frames = 0;
    bool finishing = false, interacted = false;
    std::uint16_t held = 0;
    bool run = false;
    std::uint64_t started = 0;
};
Exec g_exec;

// Drawing state.
struct Stroke {
    bool active = false;
    std::vector<std::pair<float, float>> view_points;
    std::vector<Tile> tiles;
};
Stroke g_stroke;

// Always-on step ring: every tile change the player makes while a path runs.
struct StepSample {
    std::uint64_t frame = 0;
    Tile from, to, expected;
    bool ok = true;
    char note[24] = {};
};
constexpr std::size_t kStepRing = 2048;
StepSample g_steps[kStepRing];
std::uint64_t g_step_count = 0, g_step_mismatches = 0;
std::mutex g_step_m;

void record_step(std::uint64_t frame, Tile from, Tile to, Tile expected, bool ok,
                 const char* note) {
    std::lock_guard<std::mutex> lk(g_step_m);
    StepSample& s = g_steps[g_step_count % kStepRing];
    s.frame = frame;
    s.from = from;
    s.to = to;
    s.expected = expected;
    s.ok = ok;
    std::snprintf(s.note, sizeof(s.note), "%s", note ? note : "");
    ++g_step_count;
    if (!ok) ++g_step_mismatches;
}

void set_held(Core& c, std::uint16_t keys) {
    if (g_exec.held == keys) return;
    c.synth.release(g_exec.held);
    c.synth.hold(keys);
    g_exec.held = keys;
}

void stop(Core& c, const char* why) {
    if (!g_exec.active) return;
    set_held(c, 0);
    record_action(c.frame, "path", "overworld", why, static_cast<float>(g_exec.destination.x),
                  static_cast<float>(g_exec.destination.y),
                  static_cast<int>(g_exec.path.size()));
    g_exec = Exec{};
}

// Reachability flood from the player: which tiles can be walked to, how far.
struct Reach {
    std::unordered_map<std::uint32_t, int> dist;
    static std::uint32_t key(Tile t) {
        return (static_cast<std::uint32_t>(t.x) & 0xFFFF) | (static_cast<std::uint32_t>(t.y) << 16);
    }
    bool reachable(Tile t) const { return dist.count(key(t)) != 0; }
    int distance(Tile t) const {
        auto it = dist.find(key(t));
        return it == dist.end() ? 1 << 30 : it->second;
    }
};
Reach flood(const Model& md, Tile start, int elev, bool surfing) {
    Reach r;
    struct Q { Tile t; int elev; };
    std::deque<Q> q;
    r.dist[Reach::key(start)] = 0;
    q.push_back({start, elev});
    while (!q.empty() && r.dist.size() < 12000) {
        const Q cur = q.front();
        q.pop_front();
        const int d0 = r.dist[Reach::key(cur.t)];
        for (int d = 1; d <= 4; ++d) {
            Move mv;
            if (!try_step(md, cur.t, d, cur.elev, surfing, &mv)) continue;
            if (r.dist.count(Reach::key(mv.to))) continue;
            r.dist[Reach::key(mv.to)] = d0 + 1;
            q.push_back({mv.to, mv.elevation});
        }
    }
    return r;
}

// Something that answers an A press when faced.
bool bg_event_at(const Mem& m, Tile t) {
    const std::uint32_t events = m.u32(addr::gMapHeader + 4);
    if (!m.ptr(events, 20)) return false;
    const int count = m.u8(events + 3);
    const std::uint32_t list = m.u32(events + 16);
    for (int i = 0; i < count; ++i) {
        const std::uint32_t e = list + 12u * i;
        if (m.u16(e + 0) + 7 == t.x && m.u16(e + 2) + 7 == t.y) return true;
    }
    return false;
}
bool interactive_behavior(std::uint8_t b, bool surfing) {
    if ((b >= 0x83 && b <= 0x9D) || b == 0xA0 || b == 0xB0 || b == 0xB1 || b == 0xC5 ||
        (b >= 0xE0 && b <= 0xE9))
        return true;
    // Facing water on foot offers Surf; facing a waterfall while surfing offers Waterfall.
    if (!surfing && (b == 0x10 || b == 0x11 || b == 0x12 || b == 0x14 || b == 0x15)) return true;
    return surfing && b == 0x13;
}

int facing_toward(Tile from, Tile to) {
    const int dx = to.x - from.x, dy = to.y - from.y;
    return std::abs(dx) > std::abs(dy) ? (dx < 0 ? kWest : kEast)
                                       : (dy < 0 ? kNorth : kSouth);
}

// Resolve what a tapped (or stroke-final) tile means and start executing.
// `stroke` (drawn tiles, in order) makes the route pass through its
// reachable tiles in sequence.
bool start_to(Core& c, const Mem& m, Tile target, const std::vector<Tile>& stroke) {
    Model md;
    const PlayerInfo pl = read_player(m);
    if (!md.load(m) || !pl.ok) return false;
    const bool surfing = (pl.flags & kFlagSurfing) != 0;
    const Reach reach = flood(md, pl.pos, pl.elevation, surfing);
    Exec ex;
    ex.target = target;
    ex.stroke = stroke;
    ex.map = map_id(m);
    ex.started = c.frame;

    const bool in = md.in_bounds(target.x, target.y);
    const std::uint8_t tb = in ? md.behavior(target.x, target.y) : 0xFF;
    const bool object = in && md.object_id_at(target.x, target.y) >= 0;
    const bool interactive = in && (object || bg_event_at(m, target) ||
                                    interactive_behavior(tb, surfing) || door_tile(tb));
    Tile destination = target;
    const char* decision = "walk";
    if (in && reach.reachable(target) && !object) {
        ex.goal = arrow_warp_dir(tb) != kNone ? Goal::HoldInto : Goal::Walk;
        ex.goal_dir = arrow_warp_dir(tb);
        decision = ex.goal == Goal::HoldInto ? "warp-arrow" : "walk";
    } else if (interactive) {
        // Face it from the nearest reachable neighbour (or across a counter).
        ex.goal = door_tile(tb) ? Goal::HoldInto : Goal::Interact;
        decision = ex.goal == Goal::HoldInto ? "warp-door" : "interact";
        int best = 1 << 30;
        bool found = false;
        for (int d = 1; d <= 4; ++d) {
            Tile n{target.x - kDx[d], target.y - kDy[d]};
            Tile candidates[2] = {n, n};
            int count = 1;
            if (md.in_bounds(n.x, n.y) && md.behavior(n.x, n.y) == 0x80)   // MB_COUNTER
                candidates[count++] = {n.x - kDx[d], n.y - kDy[d]};
            for (int i = 0; i < count; ++i) {
                const Tile s = candidates[i];
                if (!reach.reachable(s) || reach.distance(s) >= best) continue;
                best = reach.distance(s);
                destination = s;
                ex.goal_dir = d;   // facing from s toward target
                found = true;
            }
        }
        if (!found) {
            record_action(c.frame, "path", "overworld", "no-route", static_cast<float>(target.x),
                          static_cast<float>(target.y), 0);
            return false;
        }
    } else {
        // Scenery or out of reach: get as close as possible.
        int best_gap = 1 << 30, best_len = 1 << 30;
        for (const auto& [k2, dist] : reach.dist) {
            const Tile t{static_cast<std::int16_t>(k2 & 0xFFFF), static_cast<int>(k2 >> 16)};
            const int gap = std::abs(t.x - target.x) + std::abs(t.y - target.y);
            if (gap < best_gap || (gap == best_gap && dist < best_len)) {
                best_gap = gap;
                best_len = dist;
                destination = t;
            }
        }
        decision = "walk-nearest";
        ex.goal = Goal::Walk;
    }

    ex.active = true;
    ex.destination = destination;
    ex.last_pos = pl.pos;
    if (destination == pl.pos) {
        if (ex.goal == Goal::Walk && ex.goal_dir == kNone) return false;   // already there
        if (ex.goal_dir == kNone) ex.goal_dir = facing_toward(pl.pos, target);
        ex.finishing = true;
        g_exec = ex;
        record_action(c.frame, "path", "overworld", decision, static_cast<float>(target.x),
                      static_cast<float>(target.y), 0);
        return true;
    }

    // Route: through the drawn stroke's reachable tiles in order, then on to
    // the destination. Segments are A* over the same step model.
    std::vector<Tile> waypoints;
    for (const Tile& t : stroke)
        if (reach.reachable(t) && t != pl.pos &&
            (waypoints.empty() || waypoints.back() != t))
            waypoints.push_back(t);
    while (!waypoints.empty() && waypoints.back() != destination &&
           reach.distance(waypoints.back()) > reach.distance(destination) + 8)
        waypoints.pop_back();   // don't loop back past the goal
    waypoints.push_back(destination);
    std::vector<Move> path;
    Tile cur = pl.pos;
    int elev = pl.elevation;
    for (const Tile& w : waypoints) {
        if (w == cur) continue;
        std::vector<Move> seg = plan(md, cur, elev, surfing, {w}, stroke);
        if (seg.empty()) continue;   // unreachable from here: skip this waypoint
        for (const Move& mv : seg) path.push_back(mv);
        cur = path.back().to;
        elev = path.back().elevation;
        if (path.size() > 400) break;
    }
    if (path.empty() || path.back().to != destination) {
        std::vector<Move> direct = plan(md, pl.pos, pl.elevation, surfing, {destination}, stroke);
        if (direct.empty()) {
            record_action(c.frame, "path", "overworld", "no-route", static_cast<float>(target.x),
                          static_cast<float>(target.y), 0);
            g_exec = Exec{};
            return false;
        }
        path = std::move(direct);
    }
    ex.path = std::move(path);
    ex.run = running_allowed(m) && !(pl.flags & (kFlagMachBike | kFlagAcroBike | kFlagSurfing));
    g_exec = ex;
    record_action(c.frame, "path", "overworld", decision, static_cast<float>(target.x),
                  static_cast<float>(target.y), static_cast<int>(g_exec.path.size()));
    return true;
}

bool replan(Core& c, const Mem& m) {
    const Tile target = g_exec.target;
    const std::vector<Tile> stroke = g_exec.stroke;
    const int replans = g_exec.replans + 1;
    set_held(c, 0);
    Exec saved = g_exec;
    g_exec = Exec{};
    if (!start_to(c, m, target, stroke)) {
        g_exec = saved;
        return false;
    }
    g_exec.replans = replans;
    return true;
}

Tile expected_position() {
    if (g_exec.next < g_exec.path.size()) return g_exec.path[g_exec.next].to;
    return g_exec.destination;
}

}  // namespace

// ── Provider entry points ───────────────────────────────────────────────

bool field_tap(Core& c, const SceneState& s, const k::Gesture& g, float nx, float ny) {
    (void)g;
    if (!s.field.overworld) return false;
    const Mem m = Mem::current();
    Camera cam;
    if (!cam.load(m)) return false;
    const Tile t = cam.tile_at(nx, ny);
    const PlayerInfo pl = read_player(m);
    if (g_exec.active && pl.ok && t == pl.pos) {
        stop(c, "cancelled-by-tap");
        return true;
    }
    if (!s.field.free && !g_exec.active) return false;
    if (g_exec.active) stop(c, "retargeted");
    return start_to(c, m, t, {});
}

bool field_drag(Core& c, const SceneState& s, const k::Gesture& g) {
    if (!s.field.overworld) return false;
    if (g.kind == k::GestureKind::DragBegin) {
        if (!s.field.free && !g_exec.active) return false;
        g_stroke = {};
        g_stroke.active = true;
        g_stroke.view_points.push_back({g.start_view_x, g.start_view_y});
    }
    if (!g_stroke.active) return false;
    if (g.kind == k::GestureKind::Cancel) {
        g_stroke = {};
        return true;
    }
    g_stroke.view_points.push_back({g.view_x, g.view_y});
    if (g.kind != k::GestureKind::DragEnd) return true;

    // Finger up: rasterize the stroke into 4-connected tiles and walk it.
    const Mem m = Mem::current();
    Camera cam;
    if (!cam.load(m)) {
        g_stroke = {};
        return true;
    }
    std::vector<Tile> tiles;
    auto add = [&](Tile t) {
        if (tiles.empty() || tiles.back() != t) tiles.push_back(t);
    };
    for (const auto& [vx, vy] : g_stroke.view_points) {
        float nx = 0, ny = 0;
        view_to_native(vx, vy, &nx, &ny);
        const Tile t = cam.tile_at(nx, ny);
        if (!tiles.empty()) {
            // Fill gaps so consecutive tiles are 4-neighbours.
            Tile cur = tiles.back();
            while (std::abs(t.x - cur.x) + std::abs(t.y - cur.y) > 1) {
                if (std::abs(t.x - cur.x) >= std::abs(t.y - cur.y)) cur.x += t.x > cur.x ? 1 : -1;
                else cur.y += t.y > cur.y ? 1 : -1;
                add(cur);
            }
        }
        add(t);
    }
    g_stroke.tiles = tiles;
    g_stroke.active = false;
    if (tiles.empty()) return true;
    if (g_exec.active) stop(c, "redrawn");
    // The stroke's end decides the goal: an interactable (NPC, sign, water…)
    // is faced and used; anything else resolves to the last reachable tile
    // the finger passed over.
    Model md;
    Tile goal = tiles.back();
    if (md.load(m)) {
        const PlayerInfo pl = read_player(m);
        const bool surfing = pl.ok && (pl.flags & kFlagSurfing);
        const bool end_interactive = md.in_bounds(goal.x, goal.y) &&
            (md.object_id_at(goal.x, goal.y) >= 0 || bg_event_at(m, goal) ||
             interactive_behavior(md.behavior(goal.x, goal.y), surfing) ||
             door_tile(md.behavior(goal.x, goal.y)));
        if (!end_interactive && pl.ok) {
            const Reach reach = flood(md, pl.pos, pl.elevation, surfing);
            for (auto it = tiles.rbegin(); it != tiles.rend(); ++it) {
                if (reach.reachable(*it)) {
                    goal = *it;
                    break;
                }
            }
        }
    }
    if (!start_to(c, m, goal, tiles))
        record_action(c.frame, "drag", "overworld", "no-route", static_cast<float>(goal.x),
                      static_cast<float>(goal.y), static_cast<int>(tiles.size()));
    return true;
}

void field_frame(Core& c, FrameCtx& ctx) {
    if (!g_exec.active) return;
    const SceneState& s = ctx.scene;
    if (!s.field.overworld) {
        stop(c, g_exec.goal == Goal::HoldInto ? "warped" : "left-overworld");
        return;
    }
    const PlayerInfo pl = read_player(ctx.mem);
    if (!pl.ok) {
        stop(c, "no-player");
        return;
    }
    // Map connection crossed: every coordinate shifted; translate the plan.
    const auto map = map_id(ctx.mem);
    if (map != g_exec.map) {
        const Tile expect = expected_position();
        const int dx = pl.pos.x - expect.x, dy = pl.pos.y - expect.y;
        for (Move& mv : g_exec.path) {
            mv.to.x += dx;
            mv.to.y += dy;
        }
        g_exec.destination.x += dx;
        g_exec.destination.y += dy;
        g_exec.target.x += dx;
        g_exec.target.y += dy;
        for (Tile& t : g_exec.stroke) {
            t.x += dx;
            t.y += dy;
        }
        g_exec.last_pos = pl.pos;
        g_exec.map = map;
        record_step(c.frame, expect, pl.pos, expect, true, "connection");
    }
    // Scripts, trainers, encounters or the Start menu took control.
    if (s.field.controls_locked || s.menu.live || (pl.flags & kFlagForcedMove)) {
        stop(c, "field-locked");
        return;
    }

    if (pl.pos != g_exec.last_pos) {
        const Tile expected = expected_position();
        const bool ok = pl.pos == expected;
        record_step(c.frame, g_exec.last_pos, pl.pos, expected, ok, ok ? "step" : "mismatch");
        g_exec.last_pos = pl.pos;
        g_exec.still_frames = 0;
        // Locate the player on the plan.
        bool found = false;
        for (std::size_t i = g_exec.next; i < g_exec.path.size(); ++i) {
            if (g_exec.path[i].to == pl.pos) {
                g_exec.next = i + 1;
                found = true;
                break;
            }
        }
        if (!found && !g_exec.finishing) {
            if (!replan(c, ctx.mem)) stop(c, "off-route");
            return;
        }
    }

    if (!g_exec.finishing && g_exec.next >= g_exec.path.size() && pl.pos == g_exec.destination) {
        g_exec.finishing = true;
        g_exec.finish_frames = 0;
    }

    if (g_exec.finishing) {
        ++g_exec.finish_frames;
        if (g_exec.goal == Goal::Walk && g_exec.goal_dir == kNone) {
            stop(c, "arrived");
            return;
        }
        if (g_exec.goal == Goal::Interact) {
            if (!ctx.synth.idle()) return;
            if (pl.facing != g_exec.goal_dir && g_exec.finish_frames < 30) {
                set_held(c, 0);
                ctx.synth.tap(dir_key(g_exec.goal_dir), 1, 3);   // turn in place
                return;
            }
            if (!g_exec.interacted && pl.transition == 0) {
                set_held(c, 0);
                ctx.synth.tap(k::kGbaKeyA, 2, 2);
                g_exec.interacted = true;
                return;
            }
            if (g_exec.interacted) stop(c, "interacted");
            return;
        }
        // Warps: hold into the door / along the arrow until the map changes.
        set_held(c, dir_key(g_exec.goal_dir));
        if (g_exec.finish_frames > 90) stop(c, "warp-timeout");
        return;
    }

    // Walk: hold the direction of the next move (continuous input keeps the
    // player moving tile to tile without stopping).
    if (g_exec.next < g_exec.path.size()) {
        const Move& mv = g_exec.path[g_exec.next];
        std::uint16_t keys = dir_key(mv.dir);
        if (g_exec.run && !mv.jump) keys |= k::kGbaKeyB;
        set_held(c, keys);
    }
    if (pl.transition == 0 && pl.pos == g_exec.last_pos) {
        if (++g_exec.still_frames > 24) {
            record_step(c.frame, pl.pos, pl.pos, expected_position(), false, "blocked");
            if (g_exec.replans >= 3 || !replan(c, ctx.mem)) stop(c, "blocked");
        }
    }
}

void field_overlay(k::HostOverlay& ov) {
    const auto V = k::OverlaySpace::View;
    const Core& c = core();
    // Live stroke while drawing.
    if (g_stroke.active && g_stroke.view_points.size() >= 2) {
        std::vector<k::OverlayPoint> pts;
        for (const auto& [x, y] : g_stroke.view_points) pts.push_back({x, y});
        ov.polyline(pts.data(), pts.size(), 3.0f, {255, 255, 255, 170}, V);
    }
    if (!g_exec.active || !c.scene.field.overworld) return;
    const Mem m = Mem::current();
    Camera cam;
    if (!cam.load(m)) return;
    auto center = [&](Tile t) {
        return k::OverlayPoint{cam.tile_left(t.x) + 8.0f + c.extra_left,
                               cam.tile_top(t.y) + 8.0f + c.extra_top};
    };
    std::vector<k::OverlayPoint> pts;
    const PlayerInfo pl = read_player(m);
    if (pl.ok) pts.push_back(center(pl.pos));
    for (std::size_t i = g_exec.next; i < g_exec.path.size(); ++i) pts.push_back(center(g_exec.path[i].to));
    if (pts.size() >= 2) ov.polyline(pts.data(), pts.size(), 2.0f, {120, 235, 171, 170}, V);
    const k::OverlayPoint dest = center(g_exec.goal == Goal::Walk ? g_exec.destination : g_exec.target);
    const float pulse = 5.0f + 1.5f * std::sin(ov.host_ms() * 0.012f);
    ov.stroke_circle(dest.x, dest.y, pulse, 1.5f, {120, 235, 171, 220}, V);
}

std::string field_json() {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"active\":%s,\"goal\":\"%s\",\"target\":[%d,%d],\"destination\":[%d,%d],"
        "\"remaining\":%zu,\"replans\":%d,\"run\":%s,\"steps\":%llu,\"mismatches\":%llu,"
        "\"recent\":[",
        g_exec.active ? "true" : "false",
        g_exec.goal == Goal::Walk ? "walk" : g_exec.goal == Goal::Interact ? "interact" : "warp",
        g_exec.target.x, g_exec.target.y, g_exec.destination.x, g_exec.destination.y,
        g_exec.path.size() > g_exec.next ? g_exec.path.size() - g_exec.next : 0,
        g_exec.replans, g_exec.run ? "true" : "false",
        static_cast<unsigned long long>(g_step_count),
        static_cast<unsigned long long>(g_step_mismatches));
    out = buf;
    std::lock_guard<std::mutex> lk(g_step_m);
    const std::uint64_t n = std::min<std::uint64_t>(g_step_count, 64);
    for (std::uint64_t i = g_step_count - n; i < g_step_count; ++i) {
        const StepSample& s = g_steps[i % kStepRing];
        std::snprintf(buf, sizeof(buf),
            "%s{\"frame\":%llu,\"from\":[%d,%d],\"to\":[%d,%d],\"expected\":[%d,%d],"
            "\"ok\":%s,\"note\":\"%s\"}",
            i == g_step_count - n ? "" : ",", static_cast<unsigned long long>(s.frame),
            s.from.x, s.from.y, s.to.x, s.to.y, s.expected.x, s.expected.y,
            s.ok ? "true" : "false", s.note);
        out += buf;
    }
    out += "]}";
    return out;
}

}  // namespace emerald::touch
