/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"

#include "Transports.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Path.h"

#include "WorldPacket.h"
#include "DBCStores.h"
#include "ProgressBar.h"
#include "ScriptMgr.h"
#include "TransportSystem.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"

void MapManager::LoadTransports()
{
    QueryResult *result = WorldDatabase.Query("SELECT entry, name, period FROM transports");

    uint32 count = 0;

    if( !result )
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outString( ">> Loaded %u transports", count );
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();

        Transport *t = new Transport;

        Field *fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        std::string name = fields[1].GetCppString();
        t->m_period = fields[2].GetUInt32();

        const GameObjectInfo *goinfo = ObjectMgr::GetGameObjectInfo(entry);

        if(!goinfo)
        {
            sLog.outErrorDb("Transport ID:%u, Name: %s, will not be loaded, gameobject_template missing", entry, name.c_str());
            delete t;
            continue;
        }

        if(goinfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
        {
            sLog.outErrorDb("Transport ID:%u, Name: %s, will not be loaded, gameobject_template type wrong", entry, name.c_str());
            delete t;
            continue;
        }

        DEBUG_LOG("Loading transport %u between %s, %s transport map id %u", entry, name.c_str(), goinfo->name, goinfo->moTransport.mapID);

        std::set<uint32> mapsUsed;

        if(!t->GenerateWaypoints(goinfo->moTransport.taxiPathId, mapsUsed))
            // skip transports with empty waypoints list
        {
            sLog.outErrorDb("Transport (path id %u) path size = 0. Transport ignored, check DBC files or transport GO data0 field.",goinfo->moTransport.taxiPathId);
            delete t;
            continue;
        }

        WorldLocation loc = t->m_WayPoints[0].loc;

        //current code does not support transports in dungeon!
        const MapEntry* pMapInfo = sMapStore.LookupEntry(loc.GetMapId());
        if(!pMapInfo || pMapInfo->Instanceable())
        {
            delete t;
            continue;
        }

        // creates the Gameobject
        if (!t->Create(entry, loc.GetMapId(), loc.x, loc.y, loc.z, loc.o, GO_ANIMPROGRESS_DEFAULT, GO_DYNFLAG_LO_NONE))
        {
            delete t;
            continue;
        }

        m_Transports.insert(t);

        //If we someday decide to use the grid to track transports, here:
        Map* map = sMapMgr.CreateMap(loc.GetMapId(), t);
        t->SetMap(map);
        map->Relocation((GameObject*)t, loc);
        map->Add((GameObject*)t);
        t->Start();

        ++count;
    } while(result->NextRow());
    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u transports", count );

    // check transport data DB integrity
    result = WorldDatabase.Query("SELECT gameobject.guid,gameobject.id,transports.name FROM gameobject,transports WHERE gameobject.id = transports.entry");
    if(result)                                              // wrong data found
    {
        do
        {
            Field *fields = result->Fetch();

            uint32 guid  = fields[0].GetUInt32();
            uint32 entry = fields[1].GetUInt32();
            std::string name = fields[2].GetCppString();
            sLog.outErrorDb("Transport %u '%s' have record (GUID: %u) in `gameobject`. Transports DON'T must have any records in `gameobject` or its behavior will be unpredictable/bugged.",entry,name.c_str(),guid);
        }
        while(result->NextRow());

        delete result;
    }
}

bool MapManager::IsTransportMap(uint32 mapid) const
{
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapid);
    return mapEntry ? mapEntry->IsTransport() : false;
}

Transport const* MapManager::GetTransportByGOMapId(uint32 mapid) const
{
    for (TransportSet::const_iterator iter = m_Transports.begin(); iter != m_Transports.end(); ++iter)
    {
        Transport const* transport = *iter;

        if (!transport)
            continue;

        if (transport->GetGOInfo()->moTransport.mapID == mapid)
            return transport;
    }
    return NULL;
}

Transport* MapManager::GetTransportByGuid(ObjectGuid const& guid)
{
    for (TransportSet::iterator iter = m_Transports.begin(); iter != m_Transports.end(); ++iter)
    {
        Transport* transport = *iter;

        if (!transport)
            continue;

        if (transport->GetObjectGuid() == guid)
            return transport;
    }
    return NULL;
}

Transport::Transport() : GameObject(), m_transportKit(NULL)
{
    m_updateFlag = (UPDATEFLAG_TRANSPORT | UPDATEFLAG_HIGHGUID | UPDATEFLAG_HAS_POSITION | UPDATEFLAG_ROTATION);
}

Transport::~Transport()
{
    if (m_transportKit)
        delete m_transportKit;
}

bool Transport::Create(uint32 guidlow, uint32 mapid, float x, float y, float z, float ang, uint8 animprogress, uint16 dynamicLowValue)
{
    Relocate(WorldLocation(mapid, x, y, z, ang));
    // FIXME - instance id and phaseMask isn't set to values different from std.

    if(!IsPositionValid())
    {
        sLog.outError("Transport (GUID: %u) not created. Suggested coordinates isn't valid (X: %f Y: %f)",
            guidlow,x,y);
        return false;
    }

    Object::_Create(ObjectGuid(HIGHGUID_MO_TRANSPORT, guidlow));

    GameObjectInfo const* goinfo = ObjectMgr::GetGameObjectInfo(guidlow);

    if (!goinfo)
    {
        sLog.outErrorDb("Transport not created: entry in `gameobject_template` not found, guidlow: %u map: %u  (X: %f Y: %f Z: %f) ang: %f",guidlow, mapid, x, y, z, ang);
        return false;
    }

    m_goInfo = goinfo;

    SetObjectScale(goinfo->size);

    SetUInt32Value(GAMEOBJECT_FACTION, goinfo->faction);
    //SetUInt32Value(GAMEOBJECT_FLAGS, goinfo->flags);
    SetUInt32Value(GAMEOBJECT_FLAGS, (GO_FLAG_TRANSPORT | GO_FLAG_NODESPAWN));
    SetUInt32Value(GAMEOBJECT_LEVEL, m_period);
    SetEntry(goinfo->id);

    SetDisplayId(goinfo->displayId);

    SetGoState(GO_STATE_READY);
    SetGoType(GameobjectTypes(goinfo->type));
    SetGoArtKit(0);
    SetGoAnimProgress(animprogress);

    SetUInt16Value(GAMEOBJECT_DYNAMIC, 0, dynamicLowValue);
    SetUInt16Value(GAMEOBJECT_DYNAMIC, 1, 0);

    SetName(goinfo->name);

    m_transportKit = new TransportKit(*this);

    m_anchorageTimer.SetInterval(0);
    m_anchorageTimer.Reset();

    return true;
}

struct keyFrame
{
    explicit keyFrame(TaxiPathNodeEntry const& _node) : node(&_node),
        distSinceStop(-1.0f), distUntilStop(-1.0f), distFromPrev(-1.0f), tFrom(0.0f), tTo(0.0f)
    {
    }

    TaxiPathNodeEntry const* node;

    float distSinceStop;
    float distUntilStop;
    float distFromPrev;
    float tFrom, tTo;
};

bool Transport::GenerateWaypoints(uint32 pathid, std::set<uint32> &mapids)
{
    if (pathid >= sTaxiPathNodesByPath.size())
        return false;

    TaxiPathNodeList const& path = sTaxiPathNodesByPath[pathid];

    std::vector<keyFrame> keyFrames;
    int mapChange = 0;
    mapids.clear();
    for (size_t i = 1; i < path.size() - 1; ++i)
    {
        if (mapChange == 0)
        {
            TaxiPathNodeEntry const& node_i = path[i];
            if (node_i.mapid == path[i+1].mapid)
            {
                keyFrame k(node_i);
                keyFrames.push_back(k);
                mapids.insert(k.node->mapid);
            }
            else
            {
                mapChange = 1;
            }
        }
        else
        {
            --mapChange;
        }
    }

    int lastStop = -1;
    int firstStop = -1;

    // first cell is arrived at by teleportation :S
    keyFrames[0].distFromPrev = 0;
    if (keyFrames[0].node->actionFlag == 2)
    {
        lastStop = 0;
    }

    // find the rest of the distances between key points
    for (size_t i = 1; i < keyFrames.size(); ++i)
    {
        if ((keyFrames[i].node->actionFlag == 1) || (keyFrames[i].node->mapid != keyFrames[i-1].node->mapid))
        {
            keyFrames[i].distFromPrev = 0;
        }
        else
        {
            keyFrames[i].distFromPrev =
                sqrt(pow(keyFrames[i].node->x - keyFrames[i - 1].node->x, 2) +
                    pow(keyFrames[i].node->y - keyFrames[i - 1].node->y, 2) +
                    pow(keyFrames[i].node->z - keyFrames[i - 1].node->z, 2));
        }
        if (keyFrames[i].node->actionFlag == 2)
        {
            // remember first stop frame
            if(firstStop == -1)
                firstStop = i;
            lastStop = i;
        }
    }

    float tmpDist = 0;
    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        int j = (i + lastStop) % keyFrames.size();
        if (keyFrames[j].node->actionFlag == 2)
            tmpDist = 0;
        else
            tmpDist += keyFrames[j].distFromPrev;
        keyFrames[j].distSinceStop = tmpDist;
    }

    for (int i = int(keyFrames.size()) - 1; i >= 0; i--)
    {
        int j = (i + (firstStop+1)) % keyFrames.size();
        tmpDist += keyFrames[(j + 1) % keyFrames.size()].distFromPrev;
        keyFrames[j].distUntilStop = tmpDist;
        if (keyFrames[j].node->actionFlag == 2)
            tmpDist = 0;
    }

    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        if (keyFrames[i].distSinceStop < (30 * 30 * 0.5f))
            keyFrames[i].tFrom = sqrt(2 * keyFrames[i].distSinceStop);
        else
            keyFrames[i].tFrom = ((keyFrames[i].distSinceStop - (30 * 30 * 0.5f)) / 30) + 30;

        if (keyFrames[i].distUntilStop < (30 * 30 * 0.5f))
            keyFrames[i].tTo = sqrt(2 * keyFrames[i].distUntilStop);
        else
            keyFrames[i].tTo = ((keyFrames[i].distUntilStop - (30 * 30 * 0.5f)) / 30) + 30;

        keyFrames[i].tFrom *= 1000;
        keyFrames[i].tTo *= 1000;
    }

    //    for (int i = 0; i < keyFrames.size(); ++i) {
    //        sLog.outString("%f, %f, %f, %f, %f, %f, %f", keyFrames[i].x, keyFrames[i].y, keyFrames[i].distUntilStop, keyFrames[i].distSinceStop, keyFrames[i].distFromPrev, keyFrames[i].tFrom, keyFrames[i].tTo);
    //    }

    // Now we're completely set up; we can move along the length of each waypoint at 100 ms intervals
    // speed = max(30, t) (remember x = 0.5s^2, and when accelerating, a = 1 unit/s^2
    int t = 0;
    bool teleport = false;
    if (keyFrames[keyFrames.size() - 1].node->mapid != keyFrames[0].node->mapid)
        teleport = true;

    WayPoint pos(keyFrames[0].node->mapid, keyFrames[0].node->x, keyFrames[0].node->y, keyFrames[0].node->z, teleport,
        keyFrames[0].node->arrivalEventID, keyFrames[0].node->departureEventID);
    m_WayPoints[0] = pos;
    t += keyFrames[0].node->delay * 1000;

    uint32 cM = keyFrames[0].node->mapid;
    for (size_t i = 0; i < keyFrames.size() - 1; ++i)
    {
        float d = 0;
        float tFrom = keyFrames[i].tFrom;
        float tTo = keyFrames[i].tTo;

        // keep the generation of all these points; we use only a few now, but may need the others later
        if (((d < keyFrames[i + 1].distFromPrev) && (tTo > 0)))
        {
            while ((d < keyFrames[i + 1].distFromPrev) && (tTo > 0))
            {
                tFrom += 100;
                tTo -= 100;

                if (d > 0)
                {
                    float newX, newY, newZ;
                    newX = keyFrames[i].node->x + (keyFrames[i + 1].node->x - keyFrames[i].node->x) * d / keyFrames[i + 1].distFromPrev;
                    newY = keyFrames[i].node->y + (keyFrames[i + 1].node->y - keyFrames[i].node->y) * d / keyFrames[i + 1].distFromPrev;
                    newZ = keyFrames[i].node->z + (keyFrames[i + 1].node->z - keyFrames[i].node->z) * d / keyFrames[i + 1].distFromPrev;

                    bool teleport = false;
                    if (keyFrames[i].node->mapid != cM)
                    {
                        teleport = true;
                        cM = keyFrames[i].node->mapid;
                    }

                    //                    sLog.outString("T: %d, D: %f, x: %f, y: %f, z: %f", t, d, newX, newY, newZ);
                    WayPoint pos(keyFrames[i].node->mapid, newX, newY, newZ, teleport);
                    if (teleport)
                        m_WayPoints[t] = pos;
                }

                if (tFrom < tTo)                            // caught in tFrom dock's "gravitational pull"
                {
                    if (tFrom <= 30000)
                    {
                        d = 0.5f * (tFrom / 1000) * (tFrom / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((tFrom - 30000) / 1000);
                    }
                    d = d - keyFrames[i].distSinceStop;
                }
                else
                {
                    if (tTo <= 30000)
                    {
                        d = 0.5f * (tTo / 1000) * (tTo / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((tTo - 30000) / 1000);
                    }
                    d = keyFrames[i].distUntilStop - d;
                }
                t += 100;
            }
            t -= 100;
        }

        if (keyFrames[i + 1].tFrom > keyFrames[i + 1].tTo)
            t += 100 - ((long)keyFrames[i + 1].tTo % 100);
        else
            t += (long)keyFrames[i + 1].tTo % 100;

        bool teleport = false;
        if ((keyFrames[i + 1].node->actionFlag == 1) || (keyFrames[i + 1].node->mapid != keyFrames[i].node->mapid))
        {
            teleport = true;
            cM = keyFrames[i + 1].node->mapid;
        }

        WayPoint pos(keyFrames[i + 1].node->mapid, keyFrames[i + 1].node->x, keyFrames[i + 1].node->y, keyFrames[i + 1].node->z, teleport,
            keyFrames[i + 1].node->arrivalEventID, keyFrames[i + 1].node->departureEventID);

        //        sLog.outString("T: %d, x: %f, y: %f, z: %f, t:%d", t, pos.x, pos.y, pos.z, teleport);

        //if (teleport)
        m_WayPoints[t] = pos;

        t += keyFrames[i + 1].node->delay * 1000;
        //        sLog.outString("------");
    }

    uint32 timer = t;

    //    sLog.outDetail("    Generated %lu waypoints, total time %u.", (unsigned long)m_WayPoints.size(), timer);

    m_next = m_WayPoints.begin();                           // will used in MoveToNextWayPoint for init m_curr
    MoveToNextWayPoint();                                   // m_curr -> first point
    MoveToNextWayPoint();                                   // skip first point

    m_pathTime = timer;

    m_nextNodeTime = m_curr->first;

    return true;
}

void Transport::MoveToNextWayPoint()
{
    m_curr = m_next;

    ++m_next;
    if (m_next == m_WayPoints.end())
        m_next = m_WayPoints.begin();
}
bool Transport::AddPassenger(WorldObject* passenger)
{
    DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Passenger %s boarded transport %s", passenger->GetObjectGuid().GetString().c_str(), GetName());
    GetTransportKit()->AddPassenger(passenger);
    return true;
}

bool Transport::RemovePassenger(WorldObject* passenger)
{
    GetTransportKit()->RemovePassenger(passenger);
    DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Player %s removed from transport %s.", passenger->GetObjectGuid().GetString().c_str(), GetName());
    return true;
}

void Transport::Update(uint32 update_diff, uint32 p_time)
{
    UpdateSplineMovement(p_time);

    if (!movespline->Finalized())
        return;

    if (m_WayPoints.size() <= 1)
        return;

    bool anchorage = !m_anchorageTimer.Passed();
    if (anchorage)
    {
        m_anchorageTimer.Update(update_diff);
        if (m_anchorageTimer.Passed())
        {
            // TODO - use MovementGenerator instead this
            DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Transport::Update %s start spline movement to %f %f %f",GetObjectGuid().GetString().c_str(), m_next->second.loc.x, m_next->second.loc.y, m_next->second.loc.z);
            Movement::MoveSplineInit<GameObject*> init(*this);
            init.MoveTo((Vector3)m_next->second.loc);
            init.SetVelocity(GetGOInfo()->moTransport.moveSpeed);
            init.Launch();

            m_anchorageTimer.SetInterval(0);
            m_anchorageTimer.Reset();
        }
    }

    m_timer = WorldTimer::getMSTime() % m_period;
    while (((m_timer - m_curr->first) % m_pathTime) > ((m_next->first - m_curr->first) % m_pathTime))
    {

        // delay detect
        uint32 delta = abs(m_next->first - m_curr->first);
        if (delta > 5000)
        {
            m_anchorageTimer.SetInterval(delta);
            m_anchorageTimer.Reset();
        }

        DoEventIfAny(*m_curr,true);

        MoveToNextWayPoint();

        DoEventIfAny(*m_curr,false);


        if (SetPosition(m_curr->second.loc, m_curr->second.teleport))
        {
            if (!GetTransportKit()->IsInitialized())
                GetTransportKit()->Initialize();
            else
                // Update passenger positions
                GetTransportKit()->Update(update_diff);
        }
        else if (m_curr->second.loc.GetMapId() == m_next->second.loc.GetMapId()
            && !m_curr->second.teleport
            && m_anchorageTimer.Passed())
        {
            // TODO - use MovementGenerator instead this
            DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Transport::Update %s start spline movement to %f %f %f",GetObjectGuid().GetString().c_str(), m_next->second.loc.x, m_next->second.loc.y, m_next->second.loc.z);
            Movement::MoveSplineInit<GameObject*> init(*this);
            init.MoveTo((Vector3)m_next->second.loc);
            init.SetVelocity(GetGOInfo()->moTransport.moveSpeed);
            init.Launch();
        }

        m_nextNodeTime = m_curr->first;

        if (m_curr == m_WayPoints.begin())
            DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, " ************ BEGIN ************** %s", GetName());

        DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "%s moved to %f %f %f %d", GetName(), m_curr->second.loc.x, m_curr->second.loc.y, m_curr->second.loc.z, m_curr->second.loc.GetMapId());
    }
}

void Transport::DoEventIfAny(WayPointMap::value_type const& node, bool departure)
{
    if (uint32 eventid = departure ? node.second.departureEventID : node.second.arrivalEventID)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Taxi %s event %u of node %u of %s \"%s\") path", departure ? "departure" : "arrival", eventid, node.first, GetGuidStr().c_str(), GetName());

        if (!sScriptMgr.OnProcessEvent(eventid, this, this, departure))
            GetMap()->ScriptsStart(sEventScripts, eventid, this, this);
    }
}

void Transport::BuildStartMovePacket(Map const* targetMap)
{
    SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE);
    SetGoState(GO_STATE_ACTIVE);
}

void Transport::BuildStopMovePacket(Map const* targetMap)
{
    RemoveFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE);
    SetGoState(GO_STATE_READY);
}

uint32 Transport::GetPossibleMapByEntry(uint32 entry, bool start)
{
    GameObjectInfo const* goinfo = ObjectMgr::GetGameObjectInfo(entry);
    if (!goinfo || goinfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
        return UINT32_MAX;

    if (goinfo->moTransport.taxiPathId >= sTaxiPathNodesByPath.size())
        return UINT32_MAX;

    TaxiPathNodeList const& path = sTaxiPathNodesByPath[goinfo->moTransport.taxiPathId];

    if (path.empty())
        return UINT32_MAX;

    if (!start)
    {
        for (size_t i = 0; i < path.size(); ++i)
            if (path[i].mapid != path[0].mapid)
                return path[i].mapid;
    }

    return path[0].mapid;
}

bool Transport::IsSpawnedAtDifficulty(uint32 entry, Difficulty difficulty)
{
    GameObjectInfo const* goinfo = ObjectMgr::GetGameObjectInfo(entry);
    if (!goinfo || goinfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
        return false;
    if (!goinfo->moTransport.difficultyMask)
        return true;
    return goinfo->moTransport.difficultyMask & uint32( 1 << difficulty);
}

void Transport::Start()
{
    DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Transport::StartMovement %s (%s) start moves, period %u/%u",
        GetObjectGuid().GetString().c_str(),
        GetName(),
        m_pathTime,
        GetPeriod()
        );
    SetActiveObjectState(true);
    BuildStartMovePacket(GetMap());
}

void Transport::Stop()
{
    DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Transport::StartMovement %s (%s) stop moves, period %u/%u",
        GetObjectGuid().GetString().c_str(),
        GetName(),
        m_pathTime,
        GetPeriod()
        );
    SetActiveObjectState(false);
    BuildStopMovePacket(GetMap());
}

// Return true, only if transport has correct position!
bool Transport::SetPosition(WorldLocation const& loc, bool teleport)
{
    // prevent crash when a bad coord is sent by the client
    if (!MaNGOS::IsValidMapCoord(loc.x, loc.y, loc.z, loc.orientation))
    {
        DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Transport::SetPosition(%f, %f, %f, %f, %d) bad coordinates for transport %s!", loc.x, loc.y, loc.z, loc.orientation, teleport, GetName());
        return false;
    }

    if (teleport || GetMapId() != loc.GetMapId())
    {
        Map* oldMap = GetMap();
        Map* newMap = sMapMgr.CreateMap(loc.GetMapId(), this);

        if (!newMap)
        {
            sLog.outError("Transport::SetPosition canot create map %u for transport %s!", loc.GetMapId(), GetName());
            return false;
        }


        if (oldMap != newMap)
        {
            // Transport inserted in current map ActiveObjects list
            if (!GetTransportKit()->GetPassengers().empty())
            {
                DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Transport::SetPosition %s notify passengers (count %u) for change map from %u to %u",GetObjectGuid().GetString().c_str(), GetTransportKit()->GetPassengers().size(), GetPosition().GetMapId(), loc.GetMapId());
                GetTransportKit()->CallForAllPassengers(NotifyMapChangeBegin(oldMap, GetPosition(), loc));
            }

            oldMap->Remove((GameObject*)this, false);
            SetMap(newMap);

            newMap->Relocation((GameObject*)this, loc);
            newMap->Add((GameObject*)this);

            // Transport inserted in current map ActiveObjects list
            if (!GetTransportKit()->GetPassengers().empty())
            {
                DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES,"Transport::SetPosition %s notify passengers (count %u) for finished change map to %u",GetObjectGuid().GetString().c_str(), GetTransportKit()->GetPassengers().size(), loc.GetMapId());
                GetTransportKit()->CallForAllPassengers(NotifyMapChangeEnd(newMap,loc));
            }

            DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Transport::SetPosition %s teleported to (%f, %f, %f, %f)", GetObjectGuid().GetString().c_str(), loc.x, loc.y, loc.z, loc.orientation);
        }
        else if (!(GetPosition() == loc))
            GetMap()->Relocation((GameObject*)this, loc);
    }
    else if (!(GetPosition() == loc))
        GetMap()->Relocation((GameObject*)this, loc);


    return false;
}


/**
 * @addtogroup TransportSystem
 * @{
 *
 * @class TransportKit
 * This classe contains the code needed for MaNGOS to provide abstract support for GO transporter
 */

 
TransportKit::TransportKit(Transport& base)
    : TransportBase(&base), m_isInitialized(false)
{
}

TransportKit::~TransportKit()
{
    RemoveAllPassengers();
}

void TransportKit::Initialize()
{
    m_isInitialized = true;
}

void TransportKit::RemoveAllPassengers()
{
}

void TransportKit::Reset()
{
    m_isInitialized = true;
}

bool TransportKit::AddPassenger(WorldObject* passenger)
{
    // Calculate passengers local position
    Position pos = CalculateBoardingPositionOf(passenger->GetPosition());
    BoardPassenger(passenger, pos, -1);        // Use TransportBase to store the passenger
    if (passenger->isType(TYPEMASK_UNIT))
    {
        Unit* _passenger = (Unit*)passenger;
        _passenger->m_movementInfo.ClearTransportData();
        _passenger->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);
        _passenger->m_movementInfo.SetTransportData(GetBase()->GetObjectGuid(), pos.x, pos.y, pos.z, pos.o, 0, -1);
    }
}

void TransportKit::RemovePassenger(WorldObject* passenger)
{
    UnBoardPassenger(passenger);
    if (passenger->isType(TYPEMASK_UNIT))
    {
        Unit* _passenger = (Unit*)passenger;
        _passenger->m_movementInfo.ClearTransportData();
        _passenger->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);
    }
}

// Helper function to undo the turning of the vehicle to calculate a relative position of the passenger when boarding
Position const& TransportKit::CalculateBoardingPositionOf(Position const& pos) const override
{
    Position l(pos);
    NormalizeRotatedPosition(pos.x - GetBase()->GetPositionX(), pos.y - GetBase()->GetPositionY(), l.x, l.y);

    l.z = pos.z - GetBase()->GetPositionZ();
    l.o = MapManager::NormalizeOrientation(pos.o - GetBase()->GetOrientation());
    return l;
}

void NotifyMapChangeBegin::operator() (WorldObject* obj) const
{
    if (!obj)
        return;

    switch(obj->GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
        case TYPEID_DYNAMICOBJECT:
            break;
        case TYPEID_UNIT:
            if (obj->GetObjectGuid().IsPet())
                break;
            // TODO Despawn creatures in old map
            break;
        case TYPEID_PLAYER:
        {
            Player* plr = (Player*)obj;
            if (!plr)
                return;
            if (plr->isDead() && !plr->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                plr->ResurrectPlayer(1.0);
            if (plr->GetSession() && m_oldloc.GetMapId() != m_loc.GetMapId())
            {
                WorldPacket data(SMSG_NEW_WORLD, 4);
                data << uint32(plr->GetTransport()->GetTransportMapId());
                plr->GetSession()->SendPacket(&data);
            }
            plr->TeleportTo(m_loc, TELE_TO_NOT_LEAVE_TRANSPORT | TELE_TO_NODELAY);
            break;
        }
        // TODO - make corpse moving.
        case TYPEID_CORPSE:
        default:
            break;
    }
}

void NotifyMapChangeEnd::operator() (WorldObject* obj) const
{
    if (!obj)
        return;

    switch(obj->GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
        case TYPEID_DYNAMICOBJECT:
            break;
        case TYPEID_UNIT:
            // TODO Spawn creatures in new map
            break;
        case TYPEID_PLAYER:
            break;
        // TODO - make corpse moving.
        case TYPEID_CORPSE:
        default:
            break;
    }
}

void SendCurrentTransportDataWithHelper::operator() (WorldObject* object) const
{
    if (!object || !object->IsInWorld() || object->GetObjectGuid() == m_player->GetObjectGuid())
        return;

    if (m_player->HaveAtClient(object->GetObjectGuid()))
        object->BuildCreateUpdateBlockForPlayer(m_data, m_player);
}

