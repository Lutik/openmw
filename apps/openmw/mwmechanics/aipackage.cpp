#include "aipackage.hpp"

#include <cmath>

#include <components/esm/loadcell.hpp>
#include <components/esm/loadland.hpp>
#include <components/esm/loadmgef.hpp>

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/cellstore.hpp"
#include "creaturestats.hpp"
#include "movement.hpp"
#include "../mwworld/action.hpp"

#include "steering.hpp"
#include "actorutil.hpp"
#include "coordinateconverter.hpp"

MWMechanics::AiPackage::~AiPackage() {}

MWMechanics::AiPackage::AiPackage() : 
    mTimer(AI_REACTION_TIME + 1.0f), // to force initial pathbuild
    mShortcutProhibited(false), mShortcutFailPos()
{
}

MWWorld::Ptr MWMechanics::AiPackage::getTarget()
{
    return MWWorld::Ptr();
}

bool MWMechanics::AiPackage::sideWithTarget() const
{
    return false;
}

bool MWMechanics::AiPackage::followTargetThroughDoors() const
{
    return false;
}


bool MWMechanics::AiPackage::pathTo(const MWWorld::Ptr& actor, const ESM::Pathgrid::Point& dest, float duration, float destTolerance)
{
    //Update various Timers
    mTimer += duration; //Update timer

    ESM::Position pos = actor.getRefData().getPosition(); //position of the actor

    /// Stops the actor when it gets too close to a unloaded cell
    //... At current time, this test is unnecessary. AI shuts down when actor is more than 7168
    //... units from player, and exterior cells are 8192 units long and wide.
    //... But AI processing distance may increase in the future.
    if (isNearInactiveCell(pos))
    {
        actor.getClass().getMovementSettings(actor).mPosition[1] = 0;
        return false;
    }

    //***********************
    /// Checks if you can't get to the end position at all, adds end position to end of path
    /// Rebuilds path every [AI_REACTION_TIME] seconds, in case the target has moved
    //***********************

    ESM::Pathgrid::Point start = pos.pos;

    float distToNextWaypoint = distance(start, dest);
    bool isDestReached = (distToNextWaypoint <= destTolerance);

    if (!isDestReached && mTimer > AI_REACTION_TIME)
    {
        osg::Vec3f& lastActorPos = mLastActorPos;
        bool isStuck = distance(start, lastActorPos.x(), lastActorPos.y(), lastActorPos.z()) < actor.getClass().getSpeed(actor)*mTimer
            && distance(dest, start) > 20;

        lastActorPos = pos.asVec3();

        const ESM::Cell *cell = actor.getCell()->getCell();
        bool needPathRecalc = doesPathNeedRecalc(dest, cell); //Only rebuild path if dest point has changed

        bool isWayClear = true;

        if (!needPathRecalc) // TODO: add check if actor is actually shortcutting
        {
            isWayClear = checkWayIsClearForActor(start, dest, actor); // check if current shortcut is safe to follow
        }

        if (!isWayClear || needPathRecalc) // Only rebuild path if the target has moved or can't follow current shortcut
        {
            bool destInLOS = false;

            if (isStuck || !isWayClear || !shortcutPath(start, dest, actor, &destInLOS))
            {
                mPathFinder.buildSyncedPath(start, dest, actor.getCell());

                // give priority to go directly on target if there is minimal opportunity
                if (destInLOS && mPathFinder.getPath().size() > 1)
                {
                    // get point just before dest
                    std::list<ESM::Pathgrid::Point>::const_iterator pPointBeforeDest = mPathFinder.getPath().end();
                    --pPointBeforeDest;
                    --pPointBeforeDest;

                    // if start point is closer to the target then last point of path (excluding target itself) then go straight on the target
                    if (distance(start, dest) <= distance(dest, *pPointBeforeDest))
                    {
                        mPathFinder.clearPath();
                        mPathFinder.addPointToPath(dest);
                    }
                }
            }
        }

        if(!mPathFinder.getPath().empty()) //Path has points in it
        {
            ESM::Pathgrid::Point lastPos = mPathFinder.getPath().back(); //Get the end of the proposed path

            if(distance(dest, lastPos) > 100) //End of the path is far from the destination
                mPathFinder.addPointToPath(dest); //Adds the final destination to the path, to try to get to where you want to go
        }

        mTimer = 0;
    }

    //************************
    /// Checks if you aren't moving; attempts to unstick you
    //************************
    if (isDestReached || mPathFinder.checkPathCompleted(pos.pos[0],pos.pos[1])) //Path finished?
    {
        actor.getClass().getMovementSettings(actor).mPosition[0] = 0; // stop moving
        actor.getClass().getMovementSettings(actor).mPosition[1] = 0;
        actor.getClass().getMovementSettings(actor).mPosition[2] = 0;

        // turn to destination point
        zTurn(actor, getZAngleToPoint(start, dest));
        smoothTurn(actor, getXAngleToPoint(start, dest), 0);
        return true;
    }
    else
    {
        evadeObstacles(actor, duration, pos);
    }

    // turn to next path point by X,Z axes
    zTurn(actor, mPathFinder.getZAngleToNext(pos.pos[0], pos.pos[1]));
    smoothTurn(actor, mPathFinder.getXAngleToNext(pos.pos[0], pos.pos[1], pos.pos[2]), 0);

    return false;
}

void MWMechanics::AiPackage::evadeObstacles(const MWWorld::Ptr& actor, float duration, const ESM::Position& pos)
{
    zTurn(actor, mPathFinder.getZAngleToNext(pos.pos[0], pos.pos[1]));

    MWMechanics::Movement& movement = actor.getClass().getMovementSettings(actor);
    if (mObstacleCheck.check(actor, duration))
    {
        // first check if we're walking into a door
        MWWorld::Ptr door = getNearbyDoor(actor);
        if (door != MWWorld::Ptr()) // NOTE: checks interior cells only
        {
            if (!door.getCellRef().getTeleport() && door.getCellRef().getTrap().empty()
                    && door.getCellRef().getLockLevel() <= 0 && door.getClass().getDoorState(door) == 0) {
                MWBase::Environment::get().getWorld()->activateDoor(door, 1);
            }
        }
        else // probably walking into another NPC
        {
            mObstacleCheck.takeEvasiveAction(movement);
        }
    }
    else { //Not stuck, so reset things
        movement.mPosition[1] = 1; //Just run forward
    }
}

bool MWMechanics::AiPackage::shortcutPath(const ESM::Pathgrid::Point& startPoint, const ESM::Pathgrid::Point& endPoint, const MWWorld::Ptr& actor, bool *destInLOS)
{
    const MWWorld::Class& actorClass = actor.getClass();
    MWBase::World* world = MWBase::Environment::get().getWorld();

    // check if actor can move along z-axis
    bool actorCanMoveByZ = (actorClass.canSwim(actor) && MWBase::Environment::get().getWorld()->isSwimming(actor))
        || world->isFlying(actor);

    // don't use pathgrid when actor can move in 3 dimensions
    bool isPathClear = actorCanMoveByZ;

    if (!isPathClear
        && (!mShortcutProhibited || (PathFinder::MakeOsgVec3(mShortcutFailPos) - PathFinder::MakeOsgVec3(startPoint)).length() >= PATHFIND_SHORTCUT_RETRY_DIST))
    {
        // take the direct path only if there aren't any obstacles
        isPathClear = !MWBase::Environment::get().getWorld()->castRay(
            static_cast<float>(startPoint.mX), static_cast<float>(startPoint.mY), static_cast<float>(startPoint.mZ),
            static_cast<float>(endPoint.mX), static_cast<float>(endPoint.mY), static_cast<float>(endPoint.mZ));

        if (destInLOS != NULL) *destInLOS = isPathClear;

        if (!isPathClear)
            return false;

        // check if an actor can move along the shortcut path
        isPathClear = checkWayIsClearForActor(startPoint, endPoint, actor);
    }

    if (isPathClear) // can shortcut the path
    {
        mPathFinder.clearPath();
        mPathFinder.addPointToPath(endPoint);
        return true;
    }

    return false;
}

bool MWMechanics::AiPackage::checkWayIsClearForActor(const ESM::Pathgrid::Point& startPoint, const ESM::Pathgrid::Point& endPoint, const MWWorld::Ptr& actor)
{
    bool actorCanMoveByZ = (actor.getClass().canSwim(actor) && MWBase::Environment::get().getWorld()->isSwimming(actor))
        || MWBase::Environment::get().getWorld()->isFlying(actor);

    if (actorCanMoveByZ)
        return true;

    float actorSpeed = actor.getClass().getSpeed(actor);
    float maxAvoidDist = AI_REACTION_TIME * actorSpeed + actorSpeed / MAX_VEL_ANGULAR_RADIANS * 2; // *2 - for reliability
    osg::Vec3f::value_type distToTarget = osg::Vec3f(static_cast<float>(endPoint.mX), static_cast<float>(endPoint.mY), 0).length();

    float offsetXY = distToTarget > maxAvoidDist*1.5? maxAvoidDist : maxAvoidDist/2;

    bool isClear = checkWayIsClear(PathFinder::MakeOsgVec3(startPoint), PathFinder::MakeOsgVec3(endPoint), offsetXY);

    // update shortcut prohibit state
    if (isClear)
    {
        if (mShortcutProhibited)
        {
            mShortcutProhibited = false;
            mShortcutFailPos = ESM::Pathgrid::Point();
        }
    }
    if (!isClear)
    {
        if (mShortcutFailPos.mX == 0 && mShortcutFailPos.mY == 0 && mShortcutFailPos.mZ == 0)
        {
            mShortcutProhibited = true;
            mShortcutFailPos = startPoint;
        }
    }

    return isClear;
}

bool MWMechanics::AiPackage::doesPathNeedRecalc(ESM::Pathgrid::Point dest, const ESM::Cell *cell)
{
    bool needRecalc = distance(mPrevDest, dest) > 10;
    if (needRecalc) 
        mPrevDest = dest;

    return needRecalc;
}

bool MWMechanics::AiPackage::isTargetMagicallyHidden(const MWWorld::Ptr& target)
{
    const MagicEffects& magicEffects(target.getClass().getCreatureStats(target).getMagicEffects());
    return (magicEffects.get(ESM::MagicEffect::Invisibility).getMagnitude() > 0)
        || (magicEffects.get(ESM::MagicEffect::Chameleon).getMagnitude() > 75);
}

bool MWMechanics::AiPackage::isNearInactiveCell(const ESM::Position& actorPos)
{
    const ESM::Cell* playerCell(getPlayer().getCell()->getCell());
    if (playerCell->isExterior())
    {
        // get actor's distance from origin of center cell
        osg::Vec3f actorOffset(actorPos.asVec3());
        CoordinateConverter(playerCell).toLocal(actorOffset);

        // currently assumes 3 x 3 grid for exterior cells, with player at center cell.
        // ToDo: (Maybe) use "exterior cell load distance" setting to get count of actual active cells
        // While AI Process distance is 7168, AI shuts down actors before they reach edges of 3 x 3 grid.
        const float distanceFromEdge = 200.0;
        float minThreshold = (-1.0f * ESM::Land::REAL_SIZE) + distanceFromEdge;
        float maxThreshold = (2.0f * ESM::Land::REAL_SIZE) - distanceFromEdge;
        return (actorOffset[0] < minThreshold) || (maxThreshold < actorOffset[0])
            || (actorOffset[1] < minThreshold) || (maxThreshold < actorOffset[1]);
    }
    else
    {
        return false;
    }
}
