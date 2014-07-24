#include <ros/ros.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <race_next_best_view/GetObservationCameraPoses.h>
#include <nav_msgs/GetPlan.h>
#include <visualization_msgs/Marker.h>

#include <vector>
#include <string>
#include <algorithm>
#include <limits>

double const DRIVE_SPEED = 1.0; // [m/s]
double const TURN_SPEED = 2.0; // [rad/s]

geometry_msgs::PoseStamped frame_id_to_pose(std::string const & frame_id)
{
    geometry_msgs::PoseStamped ps;
    tf::quaternionTFToMsg(tf::createIdentityQuaternion(), ps.pose.orientation);
    ps.header.frame_id = frame_id;
    ps.header.stamp = ros::Time::now();
    return ps;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "active_explorer");
    ros::NodeHandle nh;
    tf::TransformListener tf_listener;
    ros::Publisher marker_pub(nh.advertise<visualization_msgs::Marker>("/exploration_marker", 10));
    actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> moveBaseClient("move_base", true);
    // wait for the action server to come up
    while(!moveBaseClient.waitForServer(ros::Duration(5.0))) {
        ROS_INFO("Waiting for the move_base action server to come up");
    };

    while(ros::ok()) {
        ROS_INFO("retrieving pose candidates");
        race_next_best_view::GetObservationCameraPoses pose_candidates_call;
        pose_candidates_call.request.sample_size = 500;
        pose_candidates_call.request.ray_skip = 0.95;
        //pose_candidates_call.request.roi.dimensions.x = 5;
        //pose_candidates_call.request.roi.dimensions.y = 5;
        //pose_candidates_call.request.roi.dimensions.z = 2;
        //pose_candidates_call.request.roi.pose_stamped = frame_id_to_pose("/base_footprint");
        if(!ros::service::call("/get_observation_camera_poses", pose_candidates_call)) {
            ROS_ERROR("service call failed");
            ros::Duration(5).sleep();
            continue;
        }

        ROS_INFO("evaluating pose candidate utility values");

        // get the base_footprint's current pose in kinect frame and the world frame
        tf::StampedTransform base_to_cam_tf, base_to_world_tf;
        tf_listener.lookupTransform("/head_mount_kinect_ir_link", "/base_footprint", ros::Time(0), base_to_cam_tf);
        tf_listener.lookupTransform("/map", "/base_footprint", ros::Time(0), base_to_world_tf);

        std::vector<double> candidate_utilities(pose_candidates_call.response.camera_poses.size());
        geometry_msgs::Pose best_base_pose; double best_utility = 0;
        for(unsigned int i = 0; i < candidate_utilities.size(); i++) {
            // calculate the base pose for this camera pose, assuming a static transform between them
            tf::Pose pose_candidate_tf;
            tf::poseMsgToTF(pose_candidates_call.response.camera_poses[i], pose_candidate_tf);
            geometry_msgs::Pose pose_candidate_base;
            tf::poseTFToMsg(pose_candidate_tf * base_to_cam_tf, pose_candidate_base);
            // generate path planner request
            nav_msgs::GetPlan get_plan_call;
            tf::poseTFToMsg(base_to_world_tf, get_plan_call.request.start.pose);
            get_plan_call.request.start.header.frame_id = "/map";
            get_plan_call.request.start.header.stamp = ros::Time::now();
            get_plan_call.request.goal.pose = pose_candidate_base;
            get_plan_call.request.goal.header = get_plan_call.request.start.header;
            get_plan_call.request.tolerance = 0.1;
            if(!ros::service::call("/move_base_node/make_plan", get_plan_call)) {
                ROS_ERROR("make_plan call failed");
                candidate_utilities[i] = 0;
                continue;
            }
            std::vector<geometry_msgs::PoseStamped> const & path = get_plan_call.response.plan.poses;
            if(path.empty()) {
                // no plan found
                candidate_utilities[i] = 0;
                continue;
            }
            double path_len = 0, path_curvature = 0;
            for(unsigned int i_path = 1; i_path < path.size(); i_path++) {
                tf::Pose p1, p2;
                tf::poseMsgToTF(path[i_path - 1].pose, p1);
                tf::poseMsgToTF(path[i_path].pose, p2);
                path_len += p1.getOrigin().distance(p2.getOrigin());
                path_curvature += p1.getRotation().angleShortestPath(p2.getRotation());
            }
            double exec_time = path_len / DRIVE_SPEED + path_curvature / TURN_SPEED;
            candidate_utilities[i] = pose_candidates_call.response.information_gain[i] / exec_time;
            if(candidate_utilities[i] > best_utility) {
                best_utility = candidate_utilities[i];
                best_base_pose = pose_candidate_base;
            }
        }

        if(best_utility == 0) {
            ROS_WARN("no good next best views found");
            ros::Duration(5).sleep();
            continue;
        }

        move_base_msgs::MoveBaseGoal goal;
        goal.target_pose.header.frame_id = "/map";
        goal.target_pose.header.stamp = ros::Time::now();
        goal.target_pose.pose = best_base_pose;

        visualization_msgs::Marker marker;
        marker.action = visualization_msgs::Marker::ADD;
        marker.type = visualization_msgs::Marker::ARROW;
        marker.lifetime = ros::Duration();
        marker.scale.x = 1;
        marker.scale.y = 1;
        marker.scale.z = 0.2;
        marker.color.r = 1.0;
        marker.color.g = 1.0;
        marker.color.b = 1.0;
        marker.color.a = 0.5;
        marker.pose = goal.target_pose.pose;
        marker.header = goal.target_pose.header;
        marker.id = 0;
        marker.ns = "active_explorer_debug";
        marker_pub.publish(marker);

        ROS_INFO("Sending goal");
        moveBaseClient.sendGoal(goal);
        moveBaseClient.waitForResult();
        if(moveBaseClient.getState() == actionlib::SimpleClientGoalState::SUCCEEDED) {
            ROS_INFO("goal reached with success");
            ros::Duration(7.0).sleep();
        } else {
            ROS_INFO("goal approach failed");
        }
    }
}
