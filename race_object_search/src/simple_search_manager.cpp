#include "simple_search_manager.h"

#include "observation_pose_collection.h"

#include <visualization_msgs/Marker.h>
#include <race_next_best_view/GetObservationCameraPoses.h>
#include <race_msgs/GetAnchoredObjects.h>

SimpleSearchManager::SimpleSearchManager()
:
    m_node_handle("~"),
    m_node_handle_pub(),
    m_tf_listener(),
    m_marker_pub(m_node_handle_pub.advertise<visualization_msgs::Marker>("/object_search_marker", 10000)),
    m_observe_volumes_server(m_node_handle_pub,
                             "observe_volumes",
                             boost::bind(&SimpleSearchManager::observeVolumesCb, this, _1),
                             false),
    m_agent(m_tf_listener, m_world_frame_id)
{
    m_node_handle.param("world_frame_id", m_world_frame_id, std::string("/odom_combined"));

    m_observe_volumes_server.start();
}

void SimpleSearchManager::observeVolumesCb(race_object_search::ObserveVolumesGoalConstPtr const & goal_ptr)
{
    ROS_INFO("Got a new goal!");
    race_object_search::ObserveVolumesGoal const & goal = *goal_ptr.get();
    while(ros::ok())
    {
        if(m_observe_volumes_server.isPreemptRequested())
        {
            ROS_INFO("Preempted!");
            m_observe_volumes_server.setPreempted();
            return;
        }

        // get current robot pose
        const tf::Pose robot_pose = m_agent.getCurrentRobotPose();
        const tf::Pose cam_pose = m_agent.getCurrentCamPose();

        ObservationPoseCollection opc;

        ROS_INFO("retrieving local pose candidates");
        {
            race_next_best_view::GetObservationCameraPoses pose_candidates_call;
            pose_candidates_call.request.sample_size = 100;
            pose_candidates_call.request.ray_skip = 0.75;
            pose_candidates_call.request.roi = goal.roi;
            pose_candidates_call.request.observation_position.header.frame_id = m_world_frame_id;
            pose_candidates_call.request.observation_position.header.stamp = ros::Time::now();
            tf::pointTFToMsg(m_agent.camPoseForRobotPose(robot_pose).getOrigin(),
                             pose_candidates_call.request.observation_position.point);
            pose_candidates_call.request.lock_height = false;
            if(!ros::service::call("/get_observation_camera_poses", pose_candidates_call))
            {
                ROS_ERROR("service call failed");
                ros::Duration(5).sleep();
                continue;
            }
            opc.addPoses(pose_candidates_call.response.camera_poses,
                         pose_candidates_call.response.target_points,
                         pose_candidates_call.response.cvms,
                         pose_candidates_call.response.object_sets);
        }

        ROS_INFO("retrieving global pose candidates");
        {
            race_next_best_view::GetObservationCameraPoses pose_candidates_call;
            pose_candidates_call.request.sample_size = 200;
            pose_candidates_call.request.ray_skip = 0.75;
            pose_candidates_call.request.roi = goal.roi;
            if(!ros::service::call("/get_observation_camera_poses", pose_candidates_call))
            {
                ROS_ERROR("service call failed");
                ros::Duration(5).sleep();
                continue;
            }
            opc.addPoses(pose_candidates_call.response.camera_poses,
                         pose_candidates_call.response.target_points,
                         pose_candidates_call.response.cvms,
                         pose_candidates_call.response.object_sets);
        }

        ROS_INFO("building travel time lookup table");
        ros::WallTime t0 = ros::WallTime::now();
        opc.prepareInitialTravelTimeLut(m_agent, robot_pose, cam_pose, m_world_frame_id);
        //opc.dumpInitialTravelTimeMap();
        ROS_INFO_STREAM("building initial tt lut took: " << (ros::WallTime::now()-t0).toSec());

        size_t n_pruned = opc.pruneUnreachablePoses();
        ROS_INFO_STREAM("Pruned " << n_pruned << " unreachable poses");

        ROS_INFO("evaluating pose candidate utility values");
        std::vector<double> candidate_utilities(opc.getPoses().size());
        size_t best_pose_idx;
        double best_utility = 0;
        for(size_t i = 0; i < candidate_utilities.size(); i++)
        {
            candidate_utilities[i] = opc.getPoses()[i].cell_id_sets[0].size() /
                                     opc.getInitialTravelTime(i);

            if(candidate_utilities[i] > best_utility)
            {
                best_pose_idx = i;
                best_utility = candidate_utilities[i];
            }
        }

        if(best_utility == 0)
        {
            m_observe_volumes_server.setSucceeded();
            return;
        }

        // do it
        if(m_agent.achieveCamPose(opc.getPoses()[best_pose_idx].pose,
                                    opc.getPoses()[best_pose_idx].view_distance))
        {
            // wait for acquisition
            ros::Duration(m_agent.getAcquisitionTime()).sleep();
        }
    }
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "simple_search_manager");
    SimpleSearchManager node;
    ROS_INFO("simple_search_manager: Initialized!");
    ros::spin();
}