#ifndef SIMPLE_SEARCH_MANAGER_H
#define SIMPLE_SEARCH_MANAGER_H

#include "pr2_agent.h"

#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <actionlib/server/simple_action_server.h>
#include <race_object_search/ObserveVolumesAction.h>

class SimpleSearchManager
{
public:
    SimpleSearchManager();

private:
    ros::NodeHandle m_node_handle, m_node_handle_pub;
    tf::TransformListener m_tf_listener;
    ros::Publisher m_marker_pub;
    actionlib::SimpleActionServer<race_object_search::ObserveVolumesAction> m_observe_volumes_server;
    Pr2Agent m_agent;
    std::string m_world_frame_id;

    // Callbacks
    void observeVolumesCb(race_object_search::ObserveVolumesGoalConstPtr const & goal);
};

#endif // SIMPLE_SEARCH_MANAGER_H
