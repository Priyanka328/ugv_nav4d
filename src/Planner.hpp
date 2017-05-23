#pragma once
#include <maps/grid/MLSMap.hpp>
#include <base/samples/RigidBodyState.hpp>
#include <boost/shared_ptr.hpp>
#include <base/Trajectory.hpp>
#include <motion_planning_libraries/sbpl/SbplMotionPrimitives.hpp>
#include <motion_planning_libraries/sbpl/SbplSplineMotionPrimitives.hpp>
#include "TraversabilityGenerator3d.hpp"
#include "PreComputedMotions.hpp"
#include "EnvironmentXYZTheta.hpp"

class ARAPlanner;

namespace ugv_nav4d
{

class Planner
{
    typedef EnvironmentXYZTheta::MLGrid MLSBase;
    boost::shared_ptr<EnvironmentXYZTheta> env;
    boost::shared_ptr<ARAPlanner> planner;
    
    const motion_planning_libraries::SplinePrimitivesConfig splinePrimitiveConfig; 
    const motion_planning_libraries::Mobility mobility;
    TraversabilityConfig traversabilityConfig;
    
public:
    Planner(const motion_planning_libraries::SplinePrimitivesConfig &primitiveConfig, const TraversabilityConfig &traversabilityConfig,
            const motion_planning_libraries::Mobility& mobility);
    
    template <maps::grid::MLSConfig::update_model SurfacePatch>
    void updateMap(const maps::grid::MLSMap<SurfacePatch>& mls)
    {
        boost::shared_ptr<MLSBase> mlsPtr(getMLSBase(mls));

        if(!env)
        {
            env.reset(new EnvironmentXYZTheta(mlsPtr, traversabilityConfig, splinePrimitiveConfig, mobility));
        }
        else
        {
            env->updateMap(mlsPtr);
        }
    }
    
    /** Plan a path from @p start to @p end.
     * @param maxTime Maximum processor time to use.
     * */
    bool plan(const base::Time& maxTime, const base::samples::RigidBodyState& start,
              const base::samples::RigidBodyState& end, std::vector<base::Trajectory>& resultTrajectory);
    
    
    //FIXME goalOrientationZ shouldnt be here?
    //FIXME move to env?
    /** Plan from @p start to the frontier patch closest to @p closeTo
     *  @param maxTime Maximum processor time to use.
     *  @param closeTo The closer a frontier patch is to this point the more likely it will be choosen*/
    bool planToNextFrontier(const base::Time& maxTime, const base::samples::RigidBodyState& start,
                            const base::Vector3d& closeTo, double goalOrientationZ,
                            std::vector<base::Trajectory>& resultTrajectory);
    
    void setTravConfig(const TraversabilityConfig& config);
    
    maps::grid::TraversabilityMap3d< maps::grid::TraversabilityNodeBase* >getTraversabilityMap() const;
    
    boost::shared_ptr<EnvironmentXYZTheta> getEnv() const;
    
private:
    template <class mapType>
    boost::shared_ptr<MLSBase> getMLSBase(const mapType &map)
    {
        std::cout << "Grid has size " << map.getSize().transpose() << std::endl;

        boost::shared_ptr<MLSBase> mlsPtr(new MLSBase(map));

        return mlsPtr;
    }
};

}
