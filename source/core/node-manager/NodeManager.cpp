#include "NodeManager.h"
#include "MasterNodeManager.h"

#include <node-manager/DistributedSynchroFactory.h>

#include <sstream>

using namespace sf1r;
using namespace zookeeper;

NodeManager::NodeManager()
: nodeState_(NODE_STATE_INIT)
{
}

NodeManager::~NodeManager()
{
}

void NodeManager::initWithConfig(
        const DistributedTopologyConfig& dsTopologyConfig,
        const DistributedUtilConfig& dsUtilConfig)
{
    // set distributed configurations
    dsTopologyConfig_ = dsTopologyConfig;
    dsUtilConfig_ = dsUtilConfig;

    // initialization
    NodeDef::setClusterIdNodeName(dsTopologyConfig_.clusterId_);

    initZooKeeper(dsUtilConfig_.zkConfig_.zkHosts_, dsUtilConfig_.zkConfig_.zkRecvTimeout_);

    nodeInfo_.replicaId_ = dsTopologyConfig_.curSF1Node_.replicaId_;
    nodeInfo_.nodeId_ = dsTopologyConfig_.curSF1Node_.nodeId_;
    nodeInfo_.host_ = dsTopologyConfig_.curSF1Node_.host_;
    nodeInfo_.baPort_ = dsTopologyConfig_.curSF1Node_.baPort_;

    nodePath_ = NodeDef::getNodePath(nodeInfo_.replicaId_, nodeInfo_.nodeId_);

    // initialize Master node manager if this is Master
//    if (dsTopologyConfig_.curSF1Node_.masterAgent_.enabled_)
//    {
//        MasterNodeManagerSingleton::get()->init();
//    }
}

void NodeManager::start()
{
    if (!dsTopologyConfig_.enabled_)
    {
        // not invovled in distributed deployment
        return;
    }

    if (nodeState_ == NODE_STATE_INIT)
    {
        nodeState_ = NODE_STATE_STARTING;
        enterCluster();
    }
    else
    {
        // start once
        return;
    }
}

void NodeManager::stop()
{
    leaveCluster();
}

void NodeManager::process(ZooKeeperEvent& zkEvent)
{
    std::cout<<"[NodeManager] "<< zkEvent.toString();

    if (zkEvent.type_ == ZOO_SESSION_EVENT && zkEvent.state_ == ZOO_CONNECTED_STATE)
    {
        if (nodeState_ == NODE_STATE_STARTING_WAIT_RETRY)
        {
            // retry start
            nodeState_ = NODE_STATE_STARTING;
            enterCluster();
        }
    }
}

/// private ////////////////////////////////////////////////////////////////////

void NodeManager::initZooKeeper(const std::string& zkHosts, const int recvTimeout)
{
    zookeeper_.reset(new ZooKeeper(zkHosts, recvTimeout));
    zookeeper_->registerEventHandler(this);
}

void NodeManager::initZkNameSpace()
{
    // Make sure zookeeper namaspace (znodes) is initialized properly
    // for all distributed coordination tasks.

    zookeeper_->createZNode(NodeDef::getSF1RootPath());
    // topology
    zookeeper_->createZNode(NodeDef::getSF1TopologyPath());
    std::stringstream ss;
    ss << dsTopologyConfig_.curSF1Node_.replicaId_;
    zookeeper_->createZNode(NodeDef::getReplicaPath(dsTopologyConfig_.curSF1Node_.replicaId_), ss.str());
    // synchro, todo
    zookeeper_->createZNode(NodeDef::getSynchroPath());
    DistributedSynchroFactory::initZKNodes(zookeeper_);
}

void NodeManager::enterCluster()
{
    std::cout<<"[NodeManager] staring ..."<<std::endl;

    boost::unique_lock<boost::mutex> lock(mutex_);
    if (nodeState_ == NODE_STATE_STARTED)
        return;

    // Ensure connecting to zookeeper
    if (!zookeeper_->isConnected())
    {
        zookeeper_->connect(true);

        if (!zookeeper_->isConnected())
        {
            // If still not connected, assume zookeeper service was stopped
            // and waiting for later process after zookeeper recovered.
            nodeState_ = NODE_STATE_STARTING_WAIT_RETRY;
            std::cout<<"[NodeManager] waiting for ZooKeeper Service..."<<std::endl;
            return;
        }
    }

    // Initialize zookeeper namespace for SF1 !!
    initZkNameSpace();

    // Set node info
    NodeData ndata;
    ndata.setValue(NodeData::NDATA_KEY_HOST, dsTopologyConfig_.curSF1Node_.host_);
    ndata.setValue(NodeData::NDATA_KEY_BA_PORT, dsTopologyConfig_.curSF1Node_.baPort_);
    if (dsTopologyConfig_.curSF1Node_.masterAgent_.enabled_)
    {
        ndata.setValue(NodeData::NDATA_KEY_MASTER_PORT, dsTopologyConfig_.curSF1Node_.masterAgent_.port_);
    }
    if (dsTopologyConfig_.curSF1Node_.workerAgent_.enabled_)
    {
        ndata.setValue(NodeData::NDATA_KEY_WORKER_PORT, dsTopologyConfig_.curSF1Node_.workerAgent_.port_);
        ndata.setValue(NodeData::NDATA_KEY_SHARD_ID, dsTopologyConfig_.curSF1Node_.workerAgent_.shardId_);
    }

    // Register node to zookeeper
    std::string sndata = ndata.serialize();
    if (!zookeeper_->createZNode(nodePath_, sndata, ZooKeeper::ZNODE_EPHEMERAL))
    {
        if (zookeeper_->getErrorCode() == ZooKeeper::ZERR_ZNODEEXISTS)
        {
            zookeeper_->setZNodeData(nodePath_, sndata);
            std::cout<<"[NodeManager] Conflict!! overwrote exsited node \""<<nodePath_<<"\"!"<<std::endl;
        }
        else
        {
            nodeState_ = NODE_STATE_STARTING_WAIT_RETRY;
            std::cout<<"[NodeManager] failed to start (err:"<<zookeeper_->getErrorCode()
                     <<"), waiting retry ..."<<std::endl;
            return;
        }
    }

    nodeState_ = NODE_STATE_STARTED;
    //std::cout<<"[NodeManager] node registered at \""<<nodePath_<<"\" "<<std::endl;
    std::cout<<"[NodeManager] started, "<<nodeInfo_.toString()<<std::endl;

    // Start Master manager
    if (dsTopologyConfig_.curSF1Node_.masterAgent_.enabled_)
    {
        MasterNodeManagerSingleton::get()->start();
    }
}

void NodeManager::leaveCluster()
{
    zookeeper_->deleteZNode(nodePath_, true);

    std::string replicaPath = NodeDef::getReplicaPath(nodeInfo_.replicaId_);
    std::vector<std::string> childrenList;
    zookeeper_->getZNodeChildren(replicaPath, childrenList, ZooKeeper::NOT_WATCH, false);
    if (childrenList.size() <= 0)
    {
        zookeeper_->deleteZNode(replicaPath);
    }

    childrenList.clear();
    zookeeper_->getZNodeChildren(NodeDef::getSF1TopologyPath(), childrenList, ZooKeeper::NOT_WATCH, false);
    if (childrenList.size() <= 0)
    {
        zookeeper_->deleteZNode(NodeDef::getSF1TopologyPath());
    }

    childrenList.clear();
    zookeeper_->getZNodeChildren(NodeDef::getSF1RootPath(), childrenList, ZooKeeper::NOT_WATCH, false);
    if (childrenList.size() <= 0)
    {
        zookeeper_->deleteZNode(NodeDef::getSF1RootPath());
    }
}