#pragma once

#include <maps/grid/MLSMap.hpp>
#include <boost/shared_ptr.hpp>

#include <maps/grid/TraversabilityMap3d.hpp>
#include "TraversabilityConfig.hpp"

namespace ugv_nav4d
{

    class TraversabilityGenerator3d
{
public:
    struct TrackingData
    {
        Eigen::Hyperplane<double, 3> plane;
        double slope;
        size_t id; //contiguous unique id  that can be used as index for additional metadata
    };
    
    typedef maps::grid::TraversabilityNode<TrackingData> Node;

private:
    
    typedef maps::grid::MultiLevelGridMap< maps::grid::SurfacePatchBase > MLGrid;
    typedef MLGrid::CellType Cell;
    typedef MLGrid::View View;
    typedef View::CellType ViewCell;
    typedef MLGrid::PatchType Patch;
    
    
    boost::shared_ptr<MLGrid > mlsGrid;
    maps::grid::TraversabilityMap3d<Node *> trMap;
    int currentNodeId = 0; //used while expanding
    
    bool computePlaneRansac(Node &node, const View &area);
    
    bool computePlane(Node &node, const View &area);
    
    double computeSlope(const Eigen::Hyperplane< double, int(3) >& plane) const;
    
    bool checkForObstacles(const View& area, Node* node);
    
    
    void addConnectedPatches(Node* node);

    bool getConnectedPatch(const maps::grid::Index& idx, double height, const Patch*& patch);
    
    
    TraversabilityConfig config;
public:
    TraversabilityGenerator3d(const TraversabilityConfig &config);
    ~TraversabilityGenerator3d();

    void clearTrMap();
    
    Node *generateStartNode(const Eigen::Vector3d &startPosWorld);
    void expandAll(const Eigen::Vector3d &startPosWorld);

    void expandAll(Node *startNode);

    bool expandNode(Node *node);
    
    void setMLSGrid(boost::shared_ptr<MLGrid> &grid);
    
    /**Returns the number of nodes after expansion*/
    int getNumNodes() const;
    
    const maps::grid::TraversabilityMap3d<Node *> &getTraversabilityMap() const;

    maps::grid::TraversabilityMap3d< maps::grid::TraversabilityNodeBase* > getTraversabilityBaseMap() const;
    
    /**Contains the slopes of all travnodes if debug is defined */
    mutable std::vector<Eigen::Vector4d> debugSlopes;
    
protected:
    int intersections();
};

}
