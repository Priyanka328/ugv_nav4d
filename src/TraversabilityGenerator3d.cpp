#include "TraversabilityGenerator3d.hpp"
#include <numeric/PlaneFitting.hpp>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/extract_indices.h>

#include <deque>
using namespace maps::grid;

namespace ugv_nav4d
{

TraversabilityGenerator3d::TraversabilityGenerator3d(const TraversabilityConfig& config) : config(config)
{
    trMap.setResolution(Eigen::Vector2d(config.gridResolution, config.gridResolution));
    UGV_DEBUG(
        debugData.setTravConfig(config);
        debugData.setTravGen(this);
    )
}

TraversabilityGenerator3d::~TraversabilityGenerator3d()
{
    clearTrMap();
}

const maps::grid::TraversabilityMap3d<TravGenNode *> & TraversabilityGenerator3d::getTraversabilityMap() const
{
    return trMap;
}

int TraversabilityGenerator3d::getNumNodes() const
{
    return currentNodeId;
}


bool TraversabilityGenerator3d::computePlaneRansac(TravGenNode& node, const View &area)
{
    typedef pcl::PointXYZ PointT;
    
    pcl::PointCloud<PointT>::Ptr points(new pcl::PointCloud<PointT>());
    
    Eigen::Vector2d sizeHalf(area.getSize() / 2.0);
    
    double fX = area.getSize().x() / area.getNumCells().x();
    double fY = area.getSize().y() / area.getNumCells().y();
    
    int patchCnt = 0;
    
    for(size_t y = 0; y < area.getNumCells().y(); y++)
    {
        for(size_t x = 0; x < area.getNumCells().x(); x++)
        {
            Eigen::Vector3d pos;
            pos.x() = x * fX - sizeHalf.x();
            pos.y() = y * fY - sizeHalf.y();
            
            for(const SurfacePatchBase *p : area.at(x, y))
            {
                PointT pclP;
                pclP.z = p->getTop();
                pclP.x = pos.x();
                pclP.y = pos.y();
                points->push_back(pclP);
                
                patchCnt++;
            }
        }
    }

    //if less than 3 planes -> hole
    //TODO where to implement ? here or in check obstacles ?
    if(patchCnt < 5)
    {
        //ransac will not produce a result below 5 points
        return false;
    }

    pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients ());
    pcl::PointIndices::Ptr inliers (new pcl::PointIndices ());
    // Create the segmentation object
    pcl::SACSegmentation<PointT> seg;
    // Optional
    seg.setOptimizeCoefficients (true);
    // Mandatory
    seg.setModelType (pcl::SACMODEL_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setMaxIterations (50);
    seg.setDistanceThreshold (0.1);

    // Create the filtering object
    pcl::ExtractIndices<PointT> extract;

    // Segment the largest planar component from the remaining cloud
    seg.setInputCloud (points);
    seg.segment (*inliers, *coefficients);
    if (inliers->indices.size () <= 5)
    {
//         std::cerr << "Could not estimate Ground Plane" << std::endl;
        return false;
    }


    Eigen::Vector3d normal(coefficients->values[0], coefficients->values[1], coefficients->values[2]);
    normal.normalize();
    double distToOrigin = coefficients->values[3];
    
    node.getUserData().plane = Eigen::Hyperplane<double, 3>(normal, distToOrigin);
    
    //adjust height of patch
    Eigen::ParametrizedLine<double, 3> line(Vector3d::Zero(), Eigen::Vector3d::UnitZ());
    Vector3d newPos =  line.intersectionPoint(node.getUserData().plane);
    
    if(newPos.x() > 0.0001 || newPos.y() > 0.0001)
        throw std::runtime_error("TraversabilityGenerator3d: Error, adjustement height calculation is weird");

    //remove and reeinter node
    auto &list(trMap.at(node.getIndex()));
    
    if(newPos.allFinite())
    {
        list.erase(&node);
        node.setHeight(newPos.z());
        list.insert(&node);
    }    
    
    const Eigen::Vector3d slopeDir = computeSlopeDirection(node.getUserData().plane);
    node.getUserData().slope = computeSlope(node.getUserData().plane);
    node.getUserData().slopeDirection = slopeDir;
    node.getUserData().slopeDirectionAtan2 = std::atan2(slopeDir.y(), slopeDir.x());
    
    Eigen::Vector3d pos(node.getIndex().x() * config.gridResolution, node.getIndex().y() * config.gridResolution, node.getHeight());
    pos = getTraversabilityMap().getLocalFrame().inverse(Eigen::Isometry) * pos;
    UGV_DEBUG(
        debugData.planeComputed(node);
    )

    
    return true;
}

double TraversabilityGenerator3d::computeSlope(const Eigen::Hyperplane< double, int(3) >& plane) const
{
    const Eigen::Vector3d zNormal(Eigen::Vector3d::UnitZ());
    Eigen::Vector3d planeNormal = plane.normal();
    planeNormal.normalize(); //just in case
    return acos(planeNormal.dot(zNormal));
}

Eigen::Vector3d TraversabilityGenerator3d::computeSlopeDirection(const Eigen::Hyperplane< double, int(3) >& plane) const
{
    /** The vector of maximum slope on a plane is the projection of (0,0,1) onto the plane.
     *  (0,0,1) is the steepest vector possible in the global frame, thus by projecting it onto
     *  the plane we get the steepest vector possible on that plane.
     */
    const Eigen::Vector3d zNormal(Eigen::Vector3d::UnitZ());
    const Eigen::Vector3d planeNormal(plane.normal().normalized());
    const Eigen::Vector3d projection = zNormal - zNormal.dot(planeNormal) * planeNormal;
    return projection;
}


bool TraversabilityGenerator3d::checkForObstacles(const View& area, TravGenNode *node)
{
    const Eigen::Hyperplane<double, 3> &plane(node->getUserData().plane);
    const Eigen::Vector3d planeNormal(plane.normal().normalized());
    double slope = acos(planeNormal.dot(Eigen::Vector3d::UnitZ()));
    
    if(slope > config.maxSlope)
    {
        return false;
    }
    
    
    for(size_t y = 0; y < area.getNumCells().y(); y++)
    {
        for(size_t x = 0; x < area.getNumCells().x(); x++)
        {
            Eigen::Vector3d pos;
            if(!area.fromGrid(Index(x,y), pos))
            {
                throw std::runtime_error("WTF");
            }

            //TODO the patches are ordered... Use this
            for(const SurfacePatchBase *p : area.at(x, y))
            {
                pos.z() = p->getTop();
                
                float dist = plane.signedDistance(pos);
                
                //TODO find out, of positive means above the plane....
                
                if(dist < config.robotHeight  && dist > config.maxStepHeight)
                {
                    return false;
                }
            }
        }
    }
    
    return true;
}

void TraversabilityGenerator3d::setConfig(const TraversabilityConfig &config)
{
    UGV_DEBUG(
        debugData.setTravConfig(config);
    )
    this->config = config;
}

void TraversabilityGenerator3d::expandAll(const Eigen::Vector3d& startPosWorld)
{
    TravGenNode *startNode = generateStartNode(startPosWorld);

    expandAll(startNode);
}

void TraversabilityGenerator3d::expandAll(TravGenNode* startNode)
{
    if(!startNode)
        return;

    std::deque<TravGenNode *> candidates;
    candidates.push_back(startNode);
    
    int cnd = 0;
    
    while(!candidates.empty())
    {
        TravGenNode *node = candidates.front();
        candidates.pop_front();

        //check if the node was evaluated before somehow
        if(node->isExpanded())
            continue;
        
        cnd++;
        
        if((cnd % 1000) == 0)
        {
            std::cout << "Expanded " << cnd << " nodes" << std::endl;
        }
        
        if(!expandNode(node))
        {
            continue;
        }

        for(auto *n : node->getConnections())
        {
            if(!n->isExpanded())
                candidates.push_back(static_cast<TravGenNode *>(n));
        }
    }
    
    std::cout << "Expanded " << cnd << " nodes" << std::endl;
}


void TraversabilityGenerator3d::setMLSGrid(boost::shared_ptr< MLGrid >& grid)
{
    mlsGrid = grid;
    
    std::cout << "Grid has size " << grid->getSize().transpose() << " resolution " << grid->getResolution().transpose() << std::endl;
    std::cout << "Internal resolution is " << trMap.getResolution().transpose() << std::endl;
    Vector2d newSize = grid->getSize().array() / trMap.getResolution().array();
    std::cout << "MLS was set " << grid->getResolution().transpose() << " " << mlsGrid->getResolution().transpose() << std::endl;

    std::cout << "New Size is " << newSize.transpose() << std::endl;
    
    trMap.extend(Vector2ui(newSize.x(), newSize.y()));
    
    trMap.getLocalFrame() = mlsGrid->getLocalFrame();
    
    clearTrMap();
}

void TraversabilityGenerator3d::clearTrMap()
{
    for(LevelList<TravGenNode *> &l : trMap)
    {
        for(TravGenNode *n : l)
        {
            delete n;
        }
        
        l.clear();
    }
}

TravGenNode* TraversabilityGenerator3d::generateStartNode(const Eigen::Vector3d& startPosWorld)
{
    Index idx;
    if(!trMap.toGrid(startPosWorld, idx))
    {
        std::cout << "Start position outside of map !" << std::endl;
        return nullptr;
    }

    //check if not already exists...
    auto candidates = trMap.at(idx);
    for(TravGenNode *node : candidates)
    {
        if(fabs(node->getHeight() - startPosWorld.z()) < config.maxStepHeight)
        {
            std::cout << "TraversabilityGenerator3d::generateStartNode: Using existing node " << std::endl;
            return node;
        }
    }

    
    TravGenNode *startNode = new TravGenNode(startPosWorld.z(), idx);
    startNode->getUserData().id = currentNodeId++;
    trMap.at(idx).insert(startNode);

    return startNode;
}


bool TraversabilityGenerator3d::expandNode(TravGenNode * node)
{
    Eigen::Vector3d nodePos;
    if(!trMap.fromGrid(node->getIndex(), nodePos))
        throw std::runtime_error("TraversabilityGenerator3d: Internal error node out of grid");
    
    nodePos.z() += node->getHeight();
    
    //get all surfaces in a cube of robotwidth and stepheight
    Eigen::Vector3d min(-config.robotSizeX / 2.0, -config.robotSizeX / 2.0, -config.maxStepHeight);
    Eigen::Vector3d max(-min);
    
    min += nodePos;
    max += nodePos;
    
    View intersections = mlsGrid->intersectCuboid(Eigen::AlignedBox3d(min, max));
    
    node->setExpanded();
    
    //note, computePlane must be done before checkForObstacles !
    if(!computePlaneRansac(*node, intersections))
    {
        node->setType(TraversabilityNodeBase::UNKNOWN);
        return false;
    }

    if(!checkForObstacles(intersections, node))
    {
        node->setType(TraversabilityNodeBase::OBSTACLE);
        return false;
    }
    
    node->setType(TraversabilityNodeBase::TRAVERSABLE);
    
    //add sourounding 
    addConnectedPatches(node);
    
    return true;
}

void TraversabilityGenerator3d::addConnectedPatches(TravGenNode *  node)
{  
    static std::vector<Eigen::Vector2i> surounding = {
        Eigen::Vector2i(1, 1),
        Eigen::Vector2i(1, 0),
        Eigen::Vector2i(1, -1),
        Eigen::Vector2i(0, 1),
        Eigen::Vector2i(0, -1),
        Eigen::Vector2i(-1, 1),
        Eigen::Vector2i(-1, 0),
        Eigen::Vector2i(-1, -1)};

    double curHeight = node->getHeight();
    for(const Eigen::Vector2i &motion : surounding)
    {
        const Eigen::Vector2i &idxS(motion);
        const Index idx(node->getIndex().x() + idxS.x(), node->getIndex().y() + idxS.y());
        
        if(!trMap.inGrid(idx))
        {
            continue;
        }

        //compute height of cell in respect to plane
        Vector3d patchPosPlane(idxS.x() * trMap.getResolution().x(), idxS.y() * trMap.getResolution().y(), 0);
        Eigen::ParametrizedLine<double, 3> line(patchPosPlane, Eigen::Vector3d::UnitZ());
        Vector3d newPos =  line.intersectionPoint(node->getUserData().plane);
        
        if((patchPosPlane.head(2) - newPos.head(2)).norm() > 0.001)
            throw std::runtime_error("TraversabilityGenerator3d: Error, adjustement height calculation is weird");

        //The new patch is not reachable from the current patch
        if(fabs(newPos.z() - curHeight) > config.maxStepHeight)
            continue;
        
        curHeight = newPos.z();
        
        TravGenNode *toAdd = nullptr;

        if(!newPos.allFinite())
        {
            std::cout << "newPos contains inf" << std::endl;
            continue;
        }
        
        //check if we got an existing node
        for(TravGenNode *snode : trMap.at(idx))
        {
            const double searchHeight = snode->getHeight();
            if((searchHeight - config.maxStepHeight) < curHeight && (searchHeight + config.maxStepHeight) > curHeight)
            {
                //found a connectable node
                toAdd = snode;                
                break;
            }
            
            if(searchHeight > curHeight)
                break;
        }
        
        if(!toAdd)
        {
            toAdd = new TravGenNode(curHeight, idx);
            toAdd->getUserData().id = currentNodeId++;
            trMap.at(idx).insert(toAdd);
        }

        toAdd->addConnection(node);
        node->addConnection(toAdd);
    }
}

TraversabilityMap3d< TraversabilityNodeBase *> TraversabilityGenerator3d::getTraversabilityBaseMap() const
{
    maps::grid::TraversabilityMap3d<maps::grid::TraversabilityNodeBase *> trBaseMap(trMap.getNumCells(), trMap.getResolution(), trMap.getLocalMapData());
    
    for(size_t y = 0 ; y < trMap.getNumCells().y(); y++)
    {
        for(size_t x = 0 ; x < trMap.getNumCells().x(); x++)
        {
            Index idx(x, y);
            for(auto &p : trMap.at(idx))
            {
                trBaseMap.at(idx).insert(p);
            }
        }
    }
    
    return trBaseMap;
}
}
