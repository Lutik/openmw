#include "pathfinding.hpp"
#include <limits>

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/cellstore.hpp"
#include "coordinateconverter.hpp"

namespace
{
    // Slightly cheaper version for comparisons.
    // Caller needs to be careful for very short distances (i.e. less than 1)
    // or when accumuating the results i.e. (a + b)^2 != a^2 + b^2
    //
    float distanceSquared(ESM::Pathgrid::Point point, const osg::Vec3f& pos)
    {
        return (MWMechanics::PathFinder::MakeOsgVec3(point) - pos).length2();
    }

    // Return the closest pathgrid point index from the specified position co
    // -ordinates.  NOTE: Does not check if there is a sensible way to get there
    // (e.g. a cliff in front).
    //
    // NOTE: pos is expected to be in local co-ordinates, as is grid->mPoints
    //
    int getClosestPoint(const ESM::Pathgrid* grid, const osg::Vec3f& pos)
    {
        assert(grid && !grid->mPoints.empty());

        float distanceBetween = distanceSquared(grid->mPoints[0], pos);
        int closestIndex = 0;

        // TODO: if this full scan causes performance problems mapping pathgrid
        //       points to a quadtree may help
        for(unsigned int counter = 1; counter < grid->mPoints.size(); counter++)
        {
            float potentialDistBetween = distanceSquared(grid->mPoints[counter], pos);
            if(potentialDistBetween < distanceBetween)
            {
                distanceBetween = potentialDistBetween;
                closestIndex = counter;
            }
        }

        return closestIndex;
    }

    // Chooses a reachable end pathgrid point.  start is assumed reachable.
    std::pair<int, bool> getClosestReachablePoint(const ESM::Pathgrid* grid,
                                                  const MWWorld::CellStore *cell,
                                                  const osg::Vec3f pos, int start)
    {
        assert(grid && !grid->mPoints.empty());

        float closestDistanceBetween = std::numeric_limits<float>::max();
        float closestDistanceReachable = std::numeric_limits<float>::max();
        int closestIndex = 0;
        int closestReachableIndex = 0;
        // TODO: if this full scan causes performance problems mapping pathgrid
        //       points to a quadtree may help
        for(unsigned int counter = 0; counter < grid->mPoints.size(); counter++)
        {
            float potentialDistBetween = distanceSquared(grid->mPoints[counter], pos);
            if (potentialDistBetween < closestDistanceReachable)
            {
                // found a closer one
                if (cell->isPointConnected(start, counter))
                {
                    closestDistanceReachable = potentialDistBetween;
                    closestReachableIndex = counter;
                }
                if (potentialDistBetween < closestDistanceBetween)
                {
                    closestDistanceBetween = potentialDistBetween;
                    closestIndex = counter;
                }
            }
        }

        // post-condition: start and endpoint must be connected
        assert(cell->isPointConnected(start, closestReachableIndex));

        // AiWander has logic that depends on whether a path was created, deleting
        // allowed nodes if not.  Hence a path needs to be created even if the start
        // and the end points are the same.

        return std::pair<int, bool>
            (closestReachableIndex, closestReachableIndex == closestIndex);
    }

}

namespace MWMechanics
{
    float sqrDistanceIgnoreZ(const ESM::Pathgrid::Point& point, float x, float y)
    {
        x -= point.mX;
        y -= point.mY;
        return (x * x + y * y);
    }

    float distance(const ESM::Pathgrid::Point& point, float x, float y, float z)
    {
        x -= point.mX;
        y -= point.mY;
        z -= point.mZ;
        return sqrt(x * x + y * y + z * z);
    }

    float distance(const ESM::Pathgrid::Point& a, const ESM::Pathgrid::Point& b)
    {
        float x = static_cast<float>(a.mX - b.mX);
        float y = static_cast<float>(a.mY - b.mY);
        float z = static_cast<float>(a.mZ - b.mZ);
        return sqrt(x * x + y * y + z * z);
    }

    float getZAngleToDir(const osg::Vec3f& dir)
    {
        return std::atan2(dir.x(), dir.y());
    }

    float getXAngleToDir(const osg::Vec3f& dir)
    {
        return -std::asin(dir.z() / dir.length());
    }

    float getZAngleToPoint(const ESM::Pathgrid::Point &origin, const ESM::Pathgrid::Point &dest)
    {
        osg::Vec3f dir = PathFinder::MakeOsgVec3(dest) - PathFinder::MakeOsgVec3(origin);
        return getZAngleToDir(dir);
    }

    float getXAngleToPoint(const ESM::Pathgrid::Point &origin, const ESM::Pathgrid::Point &dest)
    {
        osg::Vec3f dir = PathFinder::MakeOsgVec3(dest) - PathFinder::MakeOsgVec3(origin);
        return getXAngleToDir(dir);
    }

    bool checkWayIsClear(const osg::Vec3f& from, const osg::Vec3f& to, float offsetXY)
    {
        if((to - from).length() >= PATHFIND_CAUTION_DIST || std::abs(from.z() - to.z()) <= PATHFIND_Z_REACH)
        {
            osg::Vec3f dir = to - from;
            dir.z() = 0;
            dir.normalize();
			float verticalOffset = 200; // instead of '200' here we want the height of the actor
            osg::Vec3f _from = from + dir*offsetXY + osg::Z_AXIS * verticalOffset;

            // cast up-down ray and find height in world space of hit
            float h = _from.z() - MWBase::Environment::get().getWorld()->getDistToNearestRayHit(_from, -osg::Z_AXIS, verticalOffset + PATHFIND_Z_REACH + 1);

            if(std::abs(from.z() - h) <= PATHFIND_Z_REACH)
                return true;
        }

        return false;
    }

    PathFinder::PathFinder()
        : mPathgrid(NULL),
          mCell(NULL)
    {
    }

    void PathFinder::clearPath()
    {
        if(!mPath.empty())
            mPath.clear();
    }

    /*
     * NOTE: This method may fail to find a path.  The caller must check the
     * result before using it.  If there is no path the AI routies need to
     * implement some other heuristics to reach the target.
     *
     * NOTE: It may be desirable to simply go directly to the endPoint if for
     *       example there are no pathgrids in this cell.
     *
     * NOTE: startPoint & endPoint are in world co-ordinates
     *
     * Updates mPath using aStarSearch() or ray test (if shortcut allowed).
     * mPath consists of pathgrid points, except the last element which is
     * endPoint.  This may be useful where the endPoint is not on a pathgrid
     * point (e.g. combat).  However, if the caller has already chosen a
     * pathgrid point (e.g. wander) then it may be worth while to call
     * pop_back() to remove the redundant entry.
     *
     * NOTE: co-ordinates must be converted prior to calling getClosestPoint()
     *
     *    |
     *    |       cell
     *    |     +-----------+
     *    |     |           |
     *    |     |           |
     *    |     |      @    |
     *    |  i  |   j       |
     *    |<--->|<---->|    |
     *    |     +-----------+
     *    |   k
     *    |<---------->|         world
     *    +-----------------------------
     *
     *    i = x value of cell itself (multiply by ESM::Land::REAL_SIZE to convert)
     *    j = @.x in local co-ordinates (i.e. within the cell)
     *    k = @.x in world co-ordinates
     */
    void PathFinder::buildPath(const ESM::Pathgrid::Point &startPoint,
                               const ESM::Pathgrid::Point &endPoint,
                               const MWWorld::CellStore* cell)
    {
        mPath.clear();

        if(mCell != cell || !mPathgrid)
        {
            mCell = cell;
            mPathgrid = MWBase::Environment::get().getWorld()->getStore().get<ESM::Pathgrid>().search(*mCell->getCell());
        }

        // Refer to AiWander reseach topic on openmw forums for some background.
        // Maybe there is no pathgrid for this cell.  Just go to destination and let
        // physics take care of any blockages.
        if(!mPathgrid || mPathgrid->mPoints.empty())
        {
            mPath.push_back(endPoint);
            return;
        }

        // NOTE: getClosestPoint expects local co-ordinates
        CoordinateConverter converter(mCell->getCell());

        // NOTE: It is possible that getClosestPoint returns a pathgrind point index
        //       that is unreachable in some situations. e.g. actor is standing
        //       outside an area enclosed by walls, but there is a pathgrid
        //       point right behind the wall that is closer than any pathgrid
        //       point outside the wall
        osg::Vec3f startPointInLocalCoords(converter.toLocalVec3(startPoint));
        int startNode = getClosestPoint(mPathgrid, startPointInLocalCoords);

        osg::Vec3f endPointInLocalCoords(converter.toLocalVec3(endPoint));
        std::pair<int, bool> endNode = getClosestReachablePoint(mPathgrid, cell,
            endPointInLocalCoords,
                startNode);

        // if it's shorter for actor to travel from start to end, than to travel from either
        // start or end to nearest pathgrid point, just travel from start to end.
        float startToEndLength2 = (endPointInLocalCoords - startPointInLocalCoords).length2();
        float endTolastNodeLength2 = distanceSquared(mPathgrid->mPoints[endNode.first], endPointInLocalCoords);
        float startTo1stNodeLength2 = distanceSquared(mPathgrid->mPoints[startNode], startPointInLocalCoords);
        if ((startToEndLength2 < startTo1stNodeLength2) || (startToEndLength2 < endTolastNodeLength2))
        {
            mPath.push_back(endPoint);
            return;
        }

        // AiWander has logic that depends on whether a path was created,
        // deleting allowed nodes if not.  Hence a path needs to be created
        // even if the start and the end points are the same.
        // NOTE: aStarSearch will return an empty path if the start and end
        //       nodes are the same
        if(startNode == endNode.first)
        {
            ESM::Pathgrid::Point temp(mPathgrid->mPoints[startNode]);
            converter.toWorld(temp);
            mPath.push_back(temp);
        }
        else
        {
            mPath = mCell->aStarSearch(startNode, endNode.first);

            // convert supplied path to world co-ordinates
            for (std::list<ESM::Pathgrid::Point>::iterator iter(mPath.begin()); iter != mPath.end(); ++iter)
            {
                converter.toWorld(*iter);
            }
        }

        // If endNode found is NOT the closest PathGrid point to the endPoint,
        // assume endPoint is not reachable from endNode. In which case, 
        // path ends at endNode.
        //
        // So only add the destination (which may be different to the closest
        // pathgrid point) when endNode was the closest point to endPoint.
        //
        // This logic can fail in the opposite situate, e.g. endPoint may
        // have been reachable but happened to be very close to an
        // unreachable pathgrid point.
        //
        // The AI routines will have to deal with such situations.
        if(endNode.second)
            mPath.push_back(endPoint);
    }

    float PathFinder::getZAngleToNext(float x, float y) const
    {
        // This should never happen (programmers should have an if statement checking
        // isPathConstructed that prevents this call if otherwise).
        if(mPath.empty())
            return 0.;

        const ESM::Pathgrid::Point &nextPoint = *mPath.begin();
        float directionX = nextPoint.mX - x;
        float directionY = nextPoint.mY - y;

        return std::atan2(directionX, directionY);
    }

    float PathFinder::getXAngleToNext(float x, float y, float z) const
    {
        // This should never happen (programmers should have an if statement checking
        // isPathConstructed that prevents this call if otherwise).
        if(mPath.empty())
            return 0.;

        const ESM::Pathgrid::Point &nextPoint = *mPath.begin();
        osg::Vec3f dir = MakeOsgVec3(nextPoint) - osg::Vec3f(x,y,z);

        return -std::asin(dir.z() / dir.length());
    }

    bool PathFinder::checkPathCompleted(float x, float y, float tolerance)
    {
        if(mPath.empty())
            return true;

        const ESM::Pathgrid::Point& nextPoint = *mPath.begin();
        if (sqrDistanceIgnoreZ(nextPoint, x, y) < tolerance*tolerance)
        {
            mPath.pop_front();
            if(mPath.empty())
            {
                return true;
            }
        }

        return false;
    }

    // see header for the rationale
    void PathFinder::buildSyncedPath(const ESM::Pathgrid::Point &startPoint,
        const ESM::Pathgrid::Point &endPoint,
        const MWWorld::CellStore* cell)
    {
        if (mPath.size() < 2)
        {
            // if path has one point, then it's the destination.
            // don't need to worry about bad path for this case
            buildPath(startPoint, endPoint, cell);
        }
        else
        {
            const ESM::Pathgrid::Point oldStart(*getPath().begin());
            buildPath(startPoint, endPoint, cell);
            if (mPath.size() >= 2)
            {
                // if 2nd waypoint of new path == 1st waypoint of old, 
                // delete 1st waypoint of new path.
                std::list<ESM::Pathgrid::Point>::iterator iter = ++mPath.begin();
                if (iter->mX == oldStart.mX
                    && iter->mY == oldStart.mY
                    && iter->mZ == oldStart.mZ)
                {
                    mPath.pop_front();
                }
            }
        }
    }

}
