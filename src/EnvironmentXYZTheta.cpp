#include "EnvironmentXYZTheta.hpp"
#include <sbpl/planners/planner.h>
#include <sbpl/utils/mdpconfig.h>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <base/Pose.hpp>
#include <fstream>
#include <dwa/SubTrajectory.hpp>
#include <backward/backward.hpp>
#include <vizkit3d_debug_drawings/DebugDrawing.h>
#include <vizkit3d_debug_drawings/DebugDrawingColors.h>


backward::SignalHandling crashHandler;

using namespace std;
using namespace motion_planning_libraries;

namespace ugv_nav4d
{

const double costScaleFactor = 1000;


#define oassert(val) \
    if(!(val)) \
    {\
        std::cout << #val << std::endl; \
        std::cout << __FILE__ << ": " << __LINE__ << std::endl; \
        throw std::runtime_error("meeeeh"); \
    }



EnvironmentXYZTheta::EnvironmentXYZTheta(boost::shared_ptr<MLGrid> mlsGrid,
                                         const TraversabilityConfig& travConf,
                                         const SplinePrimitivesConfig& primitiveConfig,
                                         const Mobility& mobilityConfig) :
    travGen(travConf)
    , mlsGrid(mlsGrid)
    , availableMotions(primitiveConfig, mobilityConfig)
    , startThetaNode(nullptr)
    , startXYZNode(nullptr)
    , goalThetaNode(nullptr)
    , goalXYZNode(nullptr)
    , travConf(travConf)
    , mobilityConfig(mobilityConfig)
{
    numAngles = primitiveConfig.numAngles;
    travGen.setMLSGrid(mlsGrid);
    searchGrid.setResolution(Eigen::Vector2d(travConf.gridResolution, travConf.gridResolution));
    searchGrid.extend(travGen.getTraversabilityMap().getNumCells());
    // FIXME z2 is divided by 2.0 to avoid intersecting the floor
    robotHalfSize << travConf.robotSizeX / 2, travConf.robotSizeY / 2, travConf.robotHeight/2/2;
    
    UGV_DEBUG(
        debugData.setTravConf(travConf);
        debugData.setTravGen(&travGen);
        debugData.setMlsGrid(mlsGrid);
    )
}

void EnvironmentXYZTheta::clear()
{
    //clear the search grid
    for(maps::grid::LevelList<XYZNode *> &l : searchGrid)
    {
        for(XYZNode *n : l)
        {
            for(auto &tn : n->getUserData().thetaToNodes)
            {
                delete tn.second;
            }
            delete n;
        }
        l.clear();
    }
    searchGrid.clear();
    
    idToHash.clear();
    travNodeIdToDistance.clear();

    startThetaNode = nullptr;
    startXYZNode = nullptr;
    
    goalThetaNode = nullptr;
    goalXYZNode = nullptr;
    
    for(int *p: StateID2IndexMapping)
    {
        delete p;
    }
    StateID2IndexMapping.clear();    
}



EnvironmentXYZTheta::~EnvironmentXYZTheta()
{
    clear();
}

void EnvironmentXYZTheta::updateMap(boost::shared_ptr< EnvironmentXYZTheta::MLGrid > mlsGrid)
{
    if(this->mlsGrid && this->mlsGrid->getResolution() != mlsGrid->getResolution())
        throw std::runtime_error("EnvironmentXYZTheta::updateMap : Error got MLSMap with different resolution");
    
    travGen.setMLSGrid(mlsGrid);
    this->mlsGrid = mlsGrid;
    
    UGV_DEBUG(
        debugData.setMlsGrid(mlsGrid);
    )

    clear();
}

EnvironmentXYZTheta::XYZNode* EnvironmentXYZTheta::createNewXYZState(TravGenNode* travNode)
{
    XYZNode *xyzNode = new XYZNode(travNode->getHeight(), travNode->getIndex());
    xyzNode->getUserData().travNode = travNode;
    searchGrid.at(travNode->getIndex()).insert(xyzNode);

    return xyzNode;
}


EnvironmentXYZTheta::ThetaNode* EnvironmentXYZTheta::createNewStateFromPose(const Eigen::Vector3d& pos, double theta, XYZNode **xyzBackNode)
{
    TravGenNode *travNode = travGen.generateStartNode(pos);
    if(!travNode)
    {
        cout << "createNewStateFromPose: Error Pose " << pos.transpose() << " is out of grid" << endl;
        throw runtime_error("Pose is out of grid");
    }
    
    //must be done, to correct height of start node
    if(!travNode->isExpanded() && !travGen.expandNode(travNode))
    {
        cout << "createNewStateFromPose: Error Pose " << pos.transpose() << " is not traversable" << endl;
        throw runtime_error("Pose is not traversable");
    }
    
    XYZNode *xyzNode = createNewXYZState(travNode);
    
    DiscreteTheta thetaD(theta, numAngles);
    
    if(xyzBackNode)
        *xyzBackNode = xyzNode;
    
    return createNewState(thetaD, xyzNode);
}


void EnvironmentXYZTheta::setGoal(const Eigen::Vector3d& goalPos, double theta)
{
    if(!startXYZNode)
        throw std::runtime_error("Error, start needs to be set before goal");
    
    goalThetaNode = createNewStateFromPose(goalPos, theta, &goalXYZNode);
    
    goalXYZNode->getUserData().travNode->setNotExpanded();
    
    if(!checkOrientationAllowed(goalXYZNode->getUserData().travNode, theta))
        throw std::runtime_error("Goal orientation not allowed due to slope");
    
    if(!checkCollision(goalXYZNode->getUserData().travNode, theta))
        throw std::runtime_error("Goal inside obstacle");
    
    //NOTE If we want to precompute the heuristic (precomputeCost()) we need to expand 
    //     the whole travmap beforehand.
    travGen.expandAll(startXYZNode->getUserData().travNode);
    std::cout << "All expanded " << std::endl;
    precomputeCost();
    std::cout << "Heuristic computed" << std::endl;
}

void EnvironmentXYZTheta::setStart(const Eigen::Vector3d& startPos, double theta)
{
    startThetaNode = createNewStateFromPose(startPos, theta, &startXYZNode);
    startXYZNode->getUserData().travNode->setNotExpanded();
    
    if(!checkOrientationAllowed(startXYZNode->getUserData().travNode, theta))
        throw std::runtime_error("Start orientation not allowed due to slope");
    
}

void EnvironmentXYZTheta::SetAllPreds(CMDPSTATE* state)
{
    //implement this if the planner needs access to predecessors

    SBPL_ERROR("ERROR in EnvNAV2D... function: SetAllPreds is undefined\n");
    throw EnvironmentXYZThetaException("SetAllPreds() not implemented");
}

void EnvironmentXYZTheta::SetAllActionsandAllOutcomes(CMDPSTATE* state)
{
    SBPL_ERROR("ERROR in EnvNAV2D... function: SetAllActionsandAllOutcomes is undefined\n");
    throw EnvironmentXYZThetaException("SetAllActionsandAllOutcomes() not implemented");
}


int EnvironmentXYZTheta::GetFromToHeuristic(int FromStateID, int ToStateID)
{
    throw std::runtime_error("GetFromToHeuristic not implemented");
//     const Hash &targetHash(idToHash[ToStateID]);
//     XYZNode *targetNode = targetHash.node;
// 
//     return GetHeuristic(FromStateID, targetHash.thetaNode, targetNode);
}

maps::grid::Vector3d EnvironmentXYZTheta::getStatePosition(const int stateID) const
{
    const Hash &sourceHash(idToHash[stateID]);
    const XYZNode *node = sourceHash.node;
    maps::grid::Vector3d ret;
    travGen.getTraversabilityMap().fromGrid(node->getIndex(), ret);
    ret.z() = node->getHeight();
    return ret;
}

const Motion& EnvironmentXYZTheta::getMotion(const int fromStateID, const int toStateID)
{
    int cost = -1;
    size_t motionId = 0;
    
    vector<int> successStates;
    vector<int> successStateCosts;
    vector<size_t> motionIds;
    
    GetSuccs(fromStateID, &successStates, &successStateCosts, motionIds);
    
    for(size_t i = 0; i < successStates.size(); i++)
    {
        if(successStates[i] == toStateID)
        {
            if(cost == -1 || cost > successStateCosts[i])
            {
                cost = successStateCosts[i];
                motionId = motionIds[i];
            }
        }
    }

    if(cost == -1)
        throw std::runtime_error("Internal Error: No matching motion for output path found");
    
    return availableMotions.getMotion(motionId);
}

const vector<PoseWithCell> &EnvironmentXYZTheta::getPoses(const int fromStateID, const int toStateID)
{
    const Motion& motion = getMotion(fromStateID, toStateID);
    return motion.intermediateSteps;
}


int EnvironmentXYZTheta::GetGoalHeuristic(int stateID)
{
    const Hash &sourceHash(idToHash[stateID]);
    const XYZNode *sourceNode = sourceHash.node;
    const TravGenNode* travNode = sourceNode->getUserData().travNode;
    const ThetaNode *sourceThetaNode = sourceHash.thetaNode;
    
    const double sourceToGoalDist = travNodeIdToDistance[travNode->getUserData().id].distToGoal;
    const double timeTranslation = sourceToGoalDist / mobilityConfig.mSpeed;
    const double timeRotation = sourceThetaNode->theta.shortestDist(goalThetaNode->theta).getRadian() / mobilityConfig.mTurningSpeed;
    
    const int result = floor(std::max(timeTranslation, timeRotation) * costScaleFactor);
    oassert(result >= 0);
    return result;
}

int EnvironmentXYZTheta::GetStartHeuristic(int stateID)
{
    const Hash &targetHash(idToHash[stateID]);
    const XYZNode *targetNode = targetHash.node;
    const TravGenNode* travNode = targetNode->getUserData().travNode;
    const ThetaNode *targetThetaNode = targetHash.thetaNode;

    const double startToTargetDist = travNodeIdToDistance[travNode->getUserData().id].distToStart;
    const double timeTranslation = startToTargetDist / mobilityConfig.mSpeed;
    double timeRotation = startThetaNode->theta.shortestDist(targetThetaNode->theta).getRadian() / mobilityConfig.mTurningSpeed;
    
    const int result = floor(std::max(timeTranslation, timeRotation) * costScaleFactor);
    oassert(result >= 0);
    return result;
}

bool EnvironmentXYZTheta::InitializeEnv(const char* sEnvFile)
{
    return true;
}

bool EnvironmentXYZTheta::InitializeMDPCfg(MDPConfig* MDPCfg)
{
    if(!goalThetaNode || !startThetaNode)
        return false;
    
    //initialize MDPCfg with the start and goal ids
    MDPCfg->goalstateid = goalThetaNode->id;
    MDPCfg->startstateid = startThetaNode->id;

    return true;
}

EnvironmentXYZTheta::ThetaNode *EnvironmentXYZTheta::createNewState(const DiscreteTheta &curTheta, XYZNode *curNode)
{
    ThetaNode *newNode = new ThetaNode(curTheta);
    newNode->id = idToHash.size();
    Hash hash(curNode, newNode);
    idToHash.push_back(hash);
    curNode->getUserData().thetaToNodes.insert(make_pair(curTheta, newNode));
    
    //this structure need to be extended for every new state that is added. 
    //Is seems it is later on filled in by the planner.
    
    //insert into and initialize the mappings
    int* entry = new int[NUMOFINDICES_STATEID2IND];
    StateID2IndexMapping.push_back(entry);
    for (int i = 0; i < NUMOFINDICES_STATEID2IND; i++) {
        StateID2IndexMapping[newNode->id][i] = -1;
    }
    
    return newNode;
}

TravGenNode *EnvironmentXYZTheta::movementPossible(TravGenNode *fromTravNode, const maps::grid::Index &fromIdx, const maps::grid::Index &toIdx)
{
    if(toIdx == fromIdx)
        return fromTravNode;
    
    //get trav node associated with the next index
    TravGenNode *targetNode = fromTravNode->getConnectedNode(toIdx);
    if(!targetNode)
    {
        return nullptr;
        //FIXME this should never happen but it does on the garage map with 0.5 resolution
        throw std::runtime_error("should not happen");
    }
    
    if(!checkExpandTreadSafe(targetNode))
    {
        return nullptr;
    }
    
    //NOTE this check cannot be done before checkExpandTreadSafe because the type will be determined
    //     during the expansion. Beforehand the type is undefined
    if(targetNode->getType() != maps::grid::TraversabilityNodeBase::TRAVERSABLE &&
       targetNode->getType() != maps::grid::TraversabilityNodeBase::FRONTIER)
    {
        return nullptr;
    }  
    
        
    //TODO add additionalCosts if something is near this node etc
    return targetNode;
}

bool EnvironmentXYZTheta::checkExpandTreadSafe(TravGenNode * node)
{
    if(node->isExpanded())
    {
        return true;
    }
    
    bool result = true;
    #pragma omp critical(checkExpandTreadSafe) 
    {
        if(!node->isExpanded())
        {
            //FIXME if expandNode throws an exeception we may never unlock
            //directly returning from insde omp critical is forbidden
            result = travGen.expandNode(node);
        }
    }    
    return result;
}


void EnvironmentXYZTheta::GetSuccs(int SourceStateID, vector< int >* SuccIDV, vector< int >* CostV)
{
    std::vector<size_t> motionId;
    GetSuccs(SourceStateID, SuccIDV, CostV, motionId);
}


void EnvironmentXYZTheta::GetSuccs(int SourceStateID, vector< int >* SuccIDV, vector< int >* CostV, vector< size_t >& motionIdV)
{
    SuccIDV->clear();
    CostV->clear();
    motionIdV.clear();
    const Hash &sourceHash(idToHash[SourceStateID]);
    const XYZNode *const sourceNode = sourceHash.node;
    
    UGV_DEBUG
    (
        debugData.addSucc(sourceNode->getUserData().travNode);
    )
    
    COMPLEX_DRAWING(
        const TravGenNode* node = sourceNode->getUserData().travNode;
        Eigen::Vector3d pos((node->getIndex().x() + 0.5) * travConf.gridResolution,
                            (node->getIndex().y() + 0.5) * travConf.gridResolution,
                            node->getHeight());
        pos = mlsGrid->getLocalFrame().inverse(Eigen::Isometry) * pos;
        DRAW_SPHERE("env succs", pos, travConf.gridResolution / 2.0, base::Vector4d(1, 0, 0, 0.4));
        
    );
    
    
    
    const ThetaNode *const thetaNode = sourceHash.thetaNode;
    const maps::grid::Index sourceIndex = sourceNode->getIndex();

    
    TravGenNode *curTravNode = sourceNode->getUserData().travNode;
    if(!curTravNode->isExpanded())
    {
        //current node is not drivable
        if(!travGen.expandNode(curTravNode))
        {
            return;
        }
    }

    const auto& motions = availableMotions.getMotionForStartTheta(thetaNode->theta);
    #pragma omp parallel for schedule(dynamic, 5) if(travConf.parallelismEnabled)
    for(size_t i = 0; i < motions.size(); ++i)
    {
        const Motion &motion = motions[i];
        TravGenNode *travNode = sourceNode->getUserData().travNode;
        maps::grid::Index curIndex = sourceNode->getIndex();
        std::vector<TravGenNode*> nodesOnPath;
        bool intermediateStepsOk = true;
        for(const PoseWithCell &diff : motion.intermediateSteps)
        {
            maps::grid::Index newIndex = curIndex;
            
            //diff is always a full offset to the start position
            newIndex = sourceIndex + diff.cell;

            travNode = movementPossible(travNode, curIndex, newIndex);
            nodesOnPath.push_back(travNode);
            
            if(!travNode ||
               !checkOrientationAllowed(travNode, diff.pose.orientation))
            {
                intermediateStepsOk = false;
                break;
            }
            curIndex = newIndex;
        }
        
        if(!intermediateStepsOk)
        {
            continue;
        }
        
        maps::grid::Index finalPos(sourceIndex);
        finalPos.x() += motion.xDiff;
        finalPos.y() += motion.yDiff;
        
        travNode = movementPossible(travNode, curIndex, finalPos);
        nodesOnPath.push_back(travNode);
        if(!travNode)
            continue;
        
        if(!checkCollisions(nodesOnPath, motion))
        {
            continue;
        }
        
        //goal from source to the end of the motion was valid
        XYZNode *successXYNode = nullptr;
        ThetaNode *successthetaNode = nullptr;
        
        
        //WARNING This becomes a critical section if several motion primitives
        //        share the same finalPos.
        //        As long as this is not the case this section should be save.
        
        #pragma omp critical(searchGridAccess) 
        {
            const auto &candidateMap = searchGrid.at(finalPos);

            if(travNode->getIndex() != finalPos)
                throw std::runtime_error("Internal error, indexes do not match");
            
            XYZNode searchTmp(travNode->getHeight(), travNode->getIndex());
            
            //note, this works, as the equals check is on the height, not the node itself
            auto it = candidateMap.find(&searchTmp);

            if(it != candidateMap.end())
            {
                //found a node with a matching height
                successXYNode = *it;
            }
            else
            {
                successXYNode = createNewXYZState(travNode); //modifies searchGrid at travNode->getIndex()
            }
        }

        #pragma omp critical(thetaToNodesAccess) //TODO reduce size of critical section
        {
            const auto &thetaMap(successXYNode->getUserData().thetaToNodes);
            
            auto thetaCandidate = thetaMap.find(motion.endTheta);
            if(thetaCandidate != thetaMap.end())
            {
                successthetaNode = thetaCandidate->second;
            }
            else
            {
                successthetaNode = createNewState(motion.endTheta, successXYNode);
            }
        }
               
        double cost = 0;
        switch(travConf.slopeMetric)
        {
            case SlopeMetric::AVG_SLOPE:
            {
                const double slopeFactor = getAvgSlope(nodesOnPath) * travConf.slopeMetricScale;
                cost = motion.baseCost + motion.baseCost * slopeFactor;
                break;
            }
            case SlopeMetric::MAX_SLOPE:
            {
                const double slopeFactor = getMaxSlope(nodesOnPath) * travConf.slopeMetricScale;
                cost = motion.baseCost + motion.baseCost * slopeFactor;
                break;
            }
            case SlopeMetric::TRIANGLE_SLOPE:
            {
                //assume that the motion is a straight line, extrapolate into third dimension
                //by projecting onto a plane that connects start and end cell.
                const double heightDiff = std::abs(sourceNode->getHeight() - successXYNode->getHeight());
                //not perfect but probably more exact than the slope factors above
                const double approxMotionLen3D = std::sqrt(std::pow(motion.translationlDist, 2) + std::pow(heightDiff, 2));
                assert(approxMotionLen3D >= motion.translationlDist);//due to triangle inequality
                const double translationalVelocity = std::min(mobilityConfig.mSpeed, motion.speed);
                cost = Motion::calculateCost(approxMotionLen3D, motion.angularDist, translationalVelocity,
                                             mobilityConfig.mTurningSpeed, motion.costMultiplier);
                break;
            }
            case SlopeMetric::NONE:
                cost = motion.baseCost;
                break;
            default:
                throw std::runtime_error("unknown slope metric selected");
        }
        
        cost += travConf.costFunctionObstacleMultiplier * calcObstacleCost(nodesOnPath);
        
        oassert(int(cost) >= motion.baseCost);
        oassert(motion.baseCost > 0);
        
        const int iCost = (int)cost;
        #pragma omp critical(updateData)
        {
            SuccIDV->push_back(successthetaNode->id);
            CostV->push_back(iCost);
            motionIdV.push_back(motion.id);
        }
    } 
}

bool EnvironmentXYZTheta::checkOrientationAllowed(const TravGenNode* node,
                                const base::Orientation2D& orientationRad) const
{
    if(node->getUserData().slope < travConf.inclineLimittingMinSlope) 
        return true;
    
    const double limitRad = interpolate(node->getUserData().slope, travConf.inclineLimittingMinSlope,
                                     M_PI_2, travConf.maxSlope, travConf.inclineLimittingLimit);
    const double startRad = node->getUserData().slopeDirectionAtan2 - limitRad;
    const double width = 2 * limitRad;
    assert(width >= 0);//this happens if the travmap was generated with a different maxSlope than travConf.maxSlope
    
    const base::AngleSegment segment(base::Angle::fromRad(startRad), width);
    const base::AngleSegment segmentMirrored(base::Angle::fromRad(startRad - M_PI), width);
    const base::Angle orientation = base::Angle::fromRad(orientationRad);
    const bool isInside = segment.isInside(orientation) || segmentMirrored.isInside(orientation);

    
    UGV_DEBUG(
        debugData.orientationCheck(node, segment, segmentMirrored, orientation, isInside);
    )
    return isInside;
}

double EnvironmentXYZTheta::interpolate(double x, double x0, double y0, double x1, double y1) const
{
    //linear interpolation
    return y0 + (x - x0) * (y1 - y0)/(x1-x0);
}


bool EnvironmentXYZTheta::checkCollision(const TravGenNode* node, double zRot) const
{
    // calculate robot position in local grid coordinates
    // TODO make this a member method of GridMap
    maps::grid::Vector3d robotPosition;
    robotPosition <<
            (node->getIndex().cast<double>() + Eigen::Vector2d(0.5, 0.5)).cwiseProduct(travGen.getTraversabilityMap().getResolution()),
            node->getHeight() +  travConf.robotHeight * 0.5;
    
    const Eigen::Vector3d planeNormal = node->getUserData().plane.normal();
    oassert(planeNormal.allFinite()); 

    //FIXME names
    const Eigen::Quaterniond zRotAA( Eigen::AngleAxisd(zRot, Eigen::Vector3d::UnitZ()) ); // TODO these could be precalculated
    const Eigen::Quaterniond rotAA = Eigen::Quaterniond::FromTwoVectors(Eigen::Vector3d::UnitZ(), planeNormal);
    const Eigen::Quaterniond rotQ = rotAA * zRotAA;

    // further calculations are more efficient with rotation matrix:
    const Eigen::Matrix3d rot = rotQ.toRotationMatrix();
    
    //find min/max for bounding box
    const Eigen::Vector3d extends = rot.cwiseAbs() * robotHalfSize;
    const Eigen::Vector3d min = robotPosition - extends;
    const Eigen::Vector3d max = robotPosition + extends;
    
    const Eigen::AlignedBox3d aabb(min, max); //aabb around the rotated robot bounding box    

    
    const Eigen::Matrix3d rotInv = rot.transpose();
    bool intersects = false;
    mlsGrid->intersectAABB_callback(aabb,
        [&rotInv, &intersects, this, &robotPosition, &rotQ, &rot, &node]
        (const maps::grid::Index& idx, const maps::grid::SurfacePatchBase& p)
        {
            //FIXME this actually only tests if the top of the patch intersects with the robot
            maps::grid::Vector3d pos;
            double z = p.getMax();
            pos << (idx.cast<double>() + Eigen::Vector2d(0.5, 0.5)).cwiseProduct(this->mlsGrid->getResolution()), z;
            //transform pos into coordinate system of oriented bounding box
            pos -= robotPosition;
            pos = rotInv * pos;
            
            if((abs(pos.array()) <= this->robotHalfSize.array()).all())
            {
                //found at least one patch that is inside the oriented boundingbox
                COMPLEX_DRAWING(
                    maps::grid::Vector3d pos;
                    travGen.getTraversabilityMap().fromGrid(node->getIndex(), pos);
                    DRAW_WIREFRAME_BOX("collisions", pos, rotQ,
                                       base::Vector3d(travConf.robotSizeX, travConf.robotSizeY, travConf.robotHeight),
                                       vizkit3dDebugDrawings::Color::yellow);
                );
//                 std::cout << "COL: " << robotPosition.transpose() << std::endl;
                intersects = true;
                return true;//abort intersection check
            }
            return false; //continue intersection check
        });
    
    return !intersects;
}

bool EnvironmentXYZTheta::checkCollisions(const std::vector<TravGenNode*>& path,
                                          const Motion& motion) const
{
    //the final pose is part of the path but not of the poses.
    //Thus the size should always differ by one.
    oassert(motion.intermediateSteps.size() + 1 == path.size());
    

    for(size_t i = 0; i < path.size(); ++i)
    {
        const TravGenNode* node(path[i]);
        //path contains the final element while intermediatePoses does not.
        const double zRot = i < motion.intermediateSteps.size() ?
                            motion.intermediateSteps[i].pose.orientation :
                            motion.endTheta.getRadian();
        if(!checkCollision(node, zRot))
            return false;

    }
    
    return true;
}

Eigen::AlignedBox3d EnvironmentXYZTheta::getRobotBoundingBox() const
{
    //FIXME implement
    const Eigen::Vector3d min(0, 0, 0);
    const Eigen::Vector3d max(0.5, 1.0, 0.2);
    return Eigen::AlignedBox3d(min, max);
}

void EnvironmentXYZTheta::GetPreds(int TargetStateID, vector< int >* PredIDV, vector< int >* CostV)
{
    SBPL_ERROR("ERROR in EnvNAV2D... function: GetPreds is undefined\n");
    throw EnvironmentXYZThetaException("GetPreds() not implemented");
}

int EnvironmentXYZTheta::SizeofCreatedEnv()
{
    return static_cast<int>(idToHash.size());
}

void EnvironmentXYZTheta::PrintEnv_Config(FILE* fOut)
{
    throw EnvironmentXYZThetaException("PrintEnv_Config() not implemented");
}

void EnvironmentXYZTheta::PrintState(int stateID, bool bVerbose, FILE* fOut)
{
    const Hash &hash(idToHash[stateID]);
    
    std::stringbuf buffer;
    std::ostream os (&buffer);
    os << "State "<< stateID << " coordinate " << hash.node->getIndex().transpose() << " " << hash.node->getHeight() << " Theta " << hash.thetaNode->theta << endl;
    
    if(fOut)
        fprintf(fOut, "%s", buffer.str().c_str());
    else
        std::cout <<  buffer.str();
    
}

vector<Motion> EnvironmentXYZTheta::getMotions(const vector< int >& stateIDPath)
{
    vector<Motion> result;
    if(stateIDPath.size() >= 2)
    {
        for(size_t i = 0; i < stateIDPath.size() -1; ++i)
        {
            result.push_back(getMotion(stateIDPath[i], stateIDPath[i + 1]));
        }
    }
    return result;
}


void EnvironmentXYZTheta::getTrajectory(const vector< int >& stateIDPath, vector< base::Trajectory >& result)
{
    if(stateIDPath.size() < 2)
        return;
    
    result.clear();

    base::Trajectory curPart;

    UGV_DEBUG(
        std::vector<TravGenNode*> debugNodes;
    )
    
    for(size_t i = 0; i < stateIDPath.size() - 1; ++i)
    {
        const Motion& curMotion = getMotion(stateIDPath[i], stateIDPath[i+1]);
        const maps::grid::Vector3d start = getStatePosition(stateIDPath[i]);
        const Hash &startHash(idToHash[stateIDPath[i]]);
        const maps::grid::Index startIndex(startHash.node->getIndex());
        maps::grid::Index lastIndex = startIndex;
        TravGenNode *curNode = startHash.node->getUserData().travNode;

        
        
        std::vector<base::Vector3d> positions;
        for(const PoseWithCell &pwc : curMotion.intermediateSteps)
        {
            base::Vector3d pos(pwc.pose.position.x() + start.x(), pwc.pose.position.y() + start.y(), start.z());
            maps::grid::Index curIndex = startIndex + pwc.cell;

            if(curIndex != lastIndex)
            {
                TravGenNode *nextNode = curNode->getConnectedNode(curIndex);
                if(!nextNode)
                {
                    for(auto *n : curNode->getConnections())
                        std::cout << "Con Node " << n->getIndex().transpose() << std::endl;;
                    throw std::runtime_error("Internal error, trajectory is not continous on tr grid");
                }
                
                UGV_DEBUG(debugNodes.push_back(curNode););
                curNode = nextNode;

                lastIndex = curIndex;
            }
            
            pos.z() = curNode->getHeight();
            
            if(positions.empty() || !(positions.back().isApprox(pos)))
            {
                //need to offset by start because the poses are relative to (0/0)
                positions.emplace_back(pos);
            }
        }
        
        curPart.spline.interpolate(positions);
        curPart.speed = curMotion.type == Motion::Type::MOV_BACKWARD? -curMotion.speed : curMotion.speed;
        result.push_back(curPart);
    }
    
    //just to visualize the obstacle neighbors on the final trajectory
    UGV_DEBUG(calcObstacleCost(debugNodes););
}

maps::grid::TraversabilityMap3d< maps::grid::TraversabilityNodeBase* > EnvironmentXYZTheta::getTraversabilityBaseMap() const
{
    return travGen.getTraversabilityBaseMap();
}

const maps::grid::TraversabilityMap3d<TravGenNode*>& EnvironmentXYZTheta::getTraversabilityMap() const
{
    return travGen.getTraversabilityMap();
}

const EnvironmentXYZTheta::MLGrid& EnvironmentXYZTheta::getMlsMap() const
{
    return *mlsGrid;
}

const PreComputedMotions& EnvironmentXYZTheta::getAvailableMotions() const
{
    return availableMotions;
}

double EnvironmentXYZTheta::getAvgSlope(std::vector<TravGenNode*> path) const
{
    double slopeSum = 0;
    for(TravGenNode* node : path)
    {
        slopeSum += node->getUserData().slope; 
    }
    const double avgSlope = slopeSum / path.size();
    return avgSlope;
}

double EnvironmentXYZTheta::calcObstacleCost(std::vector<TravGenNode*> path) const
{
    //dist is in real world 
    const double neighborSquareDist = travConf.costFunctionObstacleDist * travConf.costFunctionObstacleDist;
    std::unordered_set<maps::grid::TraversabilityNodeBase*> neighbors; //all neighbors that are closer than neighborSquareDist
    
    //find all neighbors within corridor around path
    for(TravGenNode* node : path)
    {
        //vector is not the the most efficient when using std::find but for small vectors it should be ok.
        //linear serach on a cached vector is as fast as unordered_set lookup for vector sizes < 100
        // (yes, I benchmarked)
        std::deque<maps::grid::TraversabilityNodeBase*> nodes;
        std::unordered_set<maps::grid::TraversabilityNodeBase*> visited;
        nodes.push_back(node);
        const maps::grid::Vector2d nodePos = node->getIndex().cast<double>().cwiseProduct(travGen.getTraversabilityMap().getResolution());
        do
        {
            maps::grid::TraversabilityNodeBase* currentNode = nodes.front();
            nodes.pop_front();
            neighbors.insert(currentNode);
            
            for(auto neighbor : currentNode->getConnections())
            {
                //check if we have already visited this node (happens because double connected graph)
                if(visited.find(neighbor) != visited.end())
                    continue;

                visited.insert(neighbor);
                    
                //check if node is within corridor
                const maps::grid::Vector2d neighborPos = neighbor->getIndex().cast<double>().cwiseProduct(travGen.getTraversabilityMap().getResolution());
                if((neighborPos - nodePos).squaredNorm() > neighborSquareDist)
                    continue;
                
                nodes.push_back(neighbor);
            }
        }while(!nodes.empty());
    }
    
    int obstacleCount = 0;
    for(maps::grid::TraversabilityNodeBase* n : neighbors)
    {
        if(n->getType() != maps::grid::TraversabilityNodeBase::TRAVERSABLE &&
           n->getType() != maps::grid::TraversabilityNodeBase::FRONTIER)
        {
            ++obstacleCount;
        }
    }   
    
    return obstacleCount;
}

double EnvironmentXYZTheta::getMaxSlope(std::vector<TravGenNode*> path) const
{
    const TravGenNode* maxElem =  *std::max_element(path.begin(), path.end(),
                                  [] (TravGenNode* lhs, TravGenNode* rhs) 
                                  {
                                    return lhs->getUserData().slope < rhs->getUserData().slope;
                                  });
    return maxElem->getUserData().slope;
}

void EnvironmentXYZTheta::precomputeCost()
{
    std::vector<double> costToStart;
    std::vector<double> costToEnd;
    
    //FIXME test if using double max causes problems, if not, use it.
    const double maxDist = 99999; //big enough to never occur in reality. Small enough to not cause overflows when used by accident.
    
    dijkstraComputeCost(startXYZNode->getUserData().travNode, costToStart, maxDist);
    dijkstraComputeCost(goalXYZNode->getUserData().travNode, costToEnd, maxDist);
    
    assert(costToStart.size() == costToEnd.size());
    
    travNodeIdToDistance.clear();
    travNodeIdToDistance.resize(costToStart.size(), Distance(maxDist, maxDist));
    for(size_t i = 0; i < costToStart.size(); ++i)
    {
        travNodeIdToDistance[i].distToStart =  costToStart[i];
        travNodeIdToDistance[i].distToGoal =  costToEnd[i];
                
        if(i != startXYZNode->getUserData().travNode->getUserData().id &&
           i != goalXYZNode->getUserData().travNode->getUserData().id &&
           costToStart[i] <= 0)
        {
            throw std::runtime_error("Heuristic of node other than start or goal is 0");
        }
    }
}


//Adapted from: https://rosettacode.org/wiki/Dijkstra%27s_algorithm#C.2B.2B
void EnvironmentXYZTheta::dijkstraComputeCost(const TravGenNode* source,
                          std::vector<double> &outDistances, const double maxDist) const 
{
    using namespace maps::grid;
    
    outDistances.clear();
    outDistances.resize(travGen.getNumNodes(), maxDist);
    
    const int sourceId = source->getUserData().id;
    outDistances[sourceId] = 0;
    
    std::set<std::pair<double, const TravGenNode*>> vertexQ;
    vertexQ.insert(std::make_pair(outDistances[sourceId], source));
    
    UGV_DEBUG(
        debugData.clearHeuristic();
    )
    
    while (!vertexQ.empty()) 
    {
        double dist = vertexQ.begin()->first;
        const TravGenNode* u = vertexQ.begin()->second;
        
        vertexQ.erase(vertexQ.begin());
        
        const Eigen::Vector3d uPos(u->getIndex().x() * travConf.gridResolution,
                                   u->getIndex().y() * travConf.gridResolution,
                                   u->getHeight());
        
        // Visit each edge exiting u
        for(TraversabilityNodeBase *v : u->getConnections())
        {   
            //skip all non traversable nodes. They will retain the maximum cost.
            if(v->getType() != TraversabilityNodeBase::TRAVERSABLE && 
               v->getType() != TraversabilityNodeBase::FRONTIER)
                continue;
            
            TravGenNode* vCasted = static_cast<TravGenNode*>(v);
            const Eigen::Vector3d vPos(vCasted->getIndex().x() * travConf.gridResolution,
                                       vCasted->getIndex().y() * travConf.gridResolution,
                                       vCasted->getHeight());

            const double distance = getHeuristicDistance(vPos, uPos);
            double distance_through_u = dist + distance;
            const int vId = vCasted->getUserData().id;
            
            if (distance_through_u < outDistances[vId])
            {
                vertexQ.erase(std::make_pair(outDistances[vId], vCasted));
                outDistances[vId] = distance_through_u;
                vertexQ.insert(std::make_pair(outDistances[vId], vCasted));
                UGV_DEBUG(
                    debugData.addHeuristicCost(vCasted, outDistances[vId]); 
                )
            }
        }
    }
}

double EnvironmentXYZTheta::getHeuristicDistance(const Eigen::Vector3d& a, const Eigen::Vector3d& b) const
{
    switch(travConf.heuristicType)
    {
        case HeuristicType::HEURISTIC_2D:
            return (a.topRows(2) - b.topRows(2)).norm();
        case HeuristicType::HEURISTIC_3D:
            return (a - b).norm();
        default:
            throw std::runtime_error("unknown heuristic type");
    }
}

TraversabilityGenerator3d& EnvironmentXYZTheta::getTravGen()
{
    return travGen;
}

void EnvironmentXYZTheta::setTravConfig(const TraversabilityConfig& cfg)
{
    travConf = cfg;
}


}
