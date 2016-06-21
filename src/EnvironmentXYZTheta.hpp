#pragma once

#include <sbpl/discrete_space_information/environment.h>
#include <trav_gen_3d/TraversabilityGenerator3d.hpp>
#include <maps/grid/TraversabilityMap3d.hpp>

class EnvironmentXYZTheta : public DiscreteSpaceInformation
{
    TraversabilityGenerator3d travGen;
    boost::shared_ptr<maps::grid::MultiLevelGridMap<maps::grid::SurfacePatchBase> > mlsGrid;

    class PreComputedMotions;
     
    class DiscreteTheta
    {
        friend class PreComputedMotions;
        int theta;
    public:
        DiscreteTheta(int val);
        
        DiscreteTheta& operator+=(const DiscreteTheta& rhs)
        {
            theta += rhs.theta;
            return *this;
        }
        
        friend DiscreteTheta operator+(DiscreteTheta lhs,        // passing lhs by value helps optimize chained a+b+c
                     const DiscreteTheta& rhs) // otherwise, both parameters may be const references
        {
            lhs += rhs; // reuse compound assignment
            return lhs; // return the result by value (uses move constructor)
        }
        
        friend bool operator<(const DiscreteTheta& l, const DiscreteTheta& r)
        {
            return l.theta < r.theta;
        }
    };

    
    class PlannerNode
    {
        public:
            PlannerNode(const DiscreteTheta &t) :theta(t) {};
            int id;
            DiscreteTheta theta;
    };
    
    class PlannerData
    {
    public:
        TraversabilityGenerator3d::Node *travNode;
        
        ///contains alle nodes sorted by theta
        std::map<DiscreteTheta, PlannerNode *> thetaToNodes; 
    };
    
    typedef maps::grid::TraversabilityNode<PlannerData> Node;
    
    maps::grid::TraversabilityMap3d<Node *> searchGrid;
    
    class Hash 
    {
    public:
        Hash(Node *node, PlannerNode *thetaNode) : node(node), thetaNode(thetaNode)
        {
        }
        Node *node;
        PlannerNode *thetaNode;
    };
    
    std::vector<Hash> idToHash;
    
    class Motion
    {
    public:
        int xDiff;
        int yDiff;
        DiscreteTheta thetaDiff;
        
        std::vector<Eigen::Vector2i> intermediateCells;
        
        int baseCost;
    };
     
    
    class PreComputedMotions
    {
        std::vector<std::vector<Motion> > thetaToMotion;
        
    public:
        const std::vector<Motion> &getMotionForStartTheta(DiscreteTheta &theta)
        {
            return thetaToMotion[theta.theta];
        };
        
        
    };
    
    PreComputedMotions availableMotions;
    
public:
    EnvironmentXYZTheta(boost::shared_ptr<maps::grid::MultiLevelGridMap<maps::grid::SurfacePatchBase> > mlsGrid);
    virtual ~EnvironmentXYZTheta();
    
    virtual bool InitializeEnv(const char* sEnvFile);
    virtual bool InitializeMDPCfg(MDPConfig* MDPCfg);
    
    virtual int GetFromToHeuristic(int FromStateID, int ToStateID);
    virtual int GetStartHeuristic(int stateID);
    virtual int GetGoalHeuristic(int stateID);
    
    virtual void GetPreds(int TargetStateID, std::vector< int >* PredIDV, std::vector< int >* CostV);
    virtual void GetSuccs(int SourceStateID, std::vector< int >* SuccIDV, std::vector< int >* CostV);

    virtual void PrintEnv_Config(FILE* fOut);
    virtual void PrintState(int stateID, bool bVerbose, FILE* fOut = 0);

    virtual void SetAllActionsandAllOutcomes(CMDPSTATE* state);
    virtual void SetAllPreds(CMDPSTATE* state);
    virtual int SizeofCreatedEnv();
};


