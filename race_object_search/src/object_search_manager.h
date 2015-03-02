#ifndef OBJECT_SEARCH_MANAGER_H
#define OBJECT_SEARCH_MANAGER_H

#include "observation_pose_collection.h"
#include "search_plan.h"
#include "search_planner.h"

#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <actionlib/server/simple_action_server.h>
#include <race_object_search/ObserveVolumesAction.h>
#include <visualization_msgs/Marker.h>
#include <uos_active_perception_msgs/GetObservationCameraPoses.h>

#include <fstream>

template<class TAgent>
class ObjectSearchManager
{
public:
    ObjectSearchManager()
    :
        m_node_handle("~"),
        m_node_handle_pub(),
        m_tf_listener(),
        m_marker_pub(m_node_handle_pub.advertise<visualization_msgs::Marker>("/object_search_marker", 10000)),
        m_observe_volumes_server(m_node_handle_pub,
                                 "observe_volumes",
                                 boost::bind(&ObjectSearchManager::observeVolumesCb, this, _1),
                                 false),
        m_agent(m_tf_listener, m_world_frame_id)
    {
        m_node_handle.param("world_frame_id", m_world_frame_id, std::string("/odom_combined"));
        m_node_handle.param("log_dir", m_log_dir, std::string(""));
        // sampling params
        m_node_handle.param("local_sample_size", m_local_sample_size, 100);
        m_node_handle.param("global_sample_size", m_global_sample_size, 200);
        m_node_handle.param("ray_skip", m_ray_skip, 0.75);
        // planning params
        m_node_handle.param("planning_mode", m_planning_mode, std::string("simple"));
        m_node_handle.param("depth_limit", m_depth_limit, 5);
        m_node_handle.param("max_rel_branch_cost", m_max_rel_branch_cost, 1.8);
        m_node_handle.param("planning_timeout", m_planning_timeout, 20.0);

        // open logging and evaluation data files
        if(!m_log_dir.empty()) {
            std::stringstream fname;
            fname << m_log_dir << "/events.log";
            m_file_events.open(fname.str().c_str(), std::ofstream::trunc);
            fname.str("");
            fname.clear();
            fname << m_log_dir << "/vals.log";
            m_file_vals.open(fname.str().c_str(), std::ofstream::trunc);
        }

        m_observe_volumes_server.start();
        ROS_INFO_STREAM(ros::this_node::getName() << ": Initialized!");
    }

private:
    ros::NodeHandle m_node_handle, m_node_handle_pub;
    tf::TransformListener m_tf_listener;
    ros::Publisher m_marker_pub;
    actionlib::SimpleActionServer<race_object_search::ObserveVolumesAction> m_observe_volumes_server;
    TAgent m_agent;
    std::string m_world_frame_id;
    // sampling params
    int m_local_sample_size;
    int m_global_sample_size;
    double m_ray_skip;
    // planning params
    std::string m_planning_mode;
    int m_depth_limit;
    double m_max_rel_branch_cost;
    double m_planning_timeout;

    // logging and evaluation data files
    std::string m_log_dir;
    std::ofstream m_file_events;
    std::ofstream m_file_vals;

    struct EqualProbabilityCellGain
    {
         double p;
         double operator ()(uint64_t const & cell) const
         {
             return p;
         }
    };

    void logerror(const std::string & str)
    {
        ROS_ERROR_STREAM(str.c_str());
        if(m_file_events.is_open()) {
            m_file_events << ros::WallTime::now() << "\t" << str << std::endl;
        }
    }

    void loginfo(const std::string & str)
    {
        ROS_INFO_STREAM(str.c_str());
        if(m_file_events.is_open()) {
            m_file_events << ros::WallTime::now() << "\t" << str << std::endl;
        }
    }

    void logtime(const std::string & what, ros::WallTime t0)
    {
        ros::WallDuration time = ros::WallTime::now() - t0;
        ROS_INFO_STREAM("value of " << what << ": " << time);
        if(m_file_vals.is_open()) {
            m_file_vals << what << "\t" << time << std::endl;
        }
    }

    void logval(const std::string & what, double val)
    {
        ROS_INFO_STREAM("value of " << what << ": " << val);
        if(m_file_vals.is_open()) {
            m_file_vals << what << "\t" << val << std::endl;
        }
    }

    std::vector<size_t> makePlanSimple(const ObservationPoseCollection & opc)
    {
        size_t best_pose_idx;
        double best_utility = 0;
        for(size_t i = 0; i < opc.getPoses().size(); i++)
        {
            double utility = opc.getPoses()[i].cell_id_sets[0].size() / opc.getInitialTravelTime(i);
            if(utility > best_utility)
            {
                best_pose_idx = i;
                best_utility = utility;
            }
        }
        // The first value in a plan sequence is always the initial value and will be ignored, so insert 2 elements
        return std::vector<size_t>(2, best_pose_idx);
    }

    // Callbacks
    void observeVolumesCb(race_object_search::ObserveVolumesGoalConstPtr const & goal_ptr)
    {
        ROS_INFO("Got a new goal!");
        size_t iteration_counter = 0;
        std::stringstream fname;
        ros::WallTime t0;
        race_object_search::ObserveVolumesGoal const & goal = *goal_ptr.get();
        while(ros::ok())
        {
            if(m_observe_volumes_server.isPreemptRequested())
            {
                ROS_INFO("Preempted!");
                m_observe_volumes_server.setPreempted();
                return;
            }

            iteration_counter++;
            m_file_events << iteration_counter << std::endl;
            m_file_vals << iteration_counter << std::endl;

            // get current robot pose
            const tf::Pose robot_pose = m_agent.getCurrentRobotPose();
            const tf::Pose cam_pose = m_agent.getCurrentCamPose();

            ObservationPoseCollection opc;
            size_t n_cells = 0;
            double cell_volume = 0.0;

            ROS_INFO("retrieving local pose candidates");
            {
                uos_active_perception_msgs::GetObservationCameraPoses pose_candidates_call;
                pose_candidates_call.request.sample_size = m_local_sample_size;
                pose_candidates_call.request.ray_skip = m_ray_skip;
                pose_candidates_call.request.roi = goal.roi;
                pose_candidates_call.request.observation_position.header.frame_id = m_world_frame_id;
                pose_candidates_call.request.observation_position.header.stamp = ros::Time::now();
                tf::pointTFToMsg(m_agent.camPoseForRobotPose(robot_pose).getOrigin(),
                                 pose_candidates_call.request.observation_position.point);
                pose_candidates_call.request.lock_height = false;
                if(!ros::service::call("/get_observation_camera_poses", pose_candidates_call))
                {
                    logerror("get_observation_camera_poses service call failed (local samples)");
                    ros::Duration(5).sleep();
                    continue;
                }
                opc.addPoses(pose_candidates_call.response.camera_poses,
                             pose_candidates_call.response.target_points,
                             pose_candidates_call.response.cvms,
                             pose_candidates_call.response.object_sets);
                for(size_t i = 0; i < pose_candidates_call.response.roi_cell_counts.size(); ++i) {
                    n_cells += pose_candidates_call.response.roi_cell_counts[i];
                }
                cell_volume = (goal.roi[0].dimensions.x * goal.roi[0].dimensions.y * goal.roi[0].dimensions.z)
                              / pose_candidates_call.response.roi_cell_counts[0];
            }

            ROS_INFO("retrieving global pose candidates");
            {
                uos_active_perception_msgs::GetObservationCameraPoses pose_candidates_call;
                pose_candidates_call.request.sample_size = m_global_sample_size;
                pose_candidates_call.request.ray_skip = m_ray_skip;
                pose_candidates_call.request.roi = goal.roi;
                if(!ros::service::call("/get_observation_camera_poses", pose_candidates_call))
                {
                    logerror("get_observation_camera_poses service call failed (global samples)");
                    ros::Duration(5).sleep();
                    continue;
                }
                opc.addPoses(pose_candidates_call.response.camera_poses,
                             pose_candidates_call.response.target_points,
                             pose_candidates_call.response.cvms,
                             pose_candidates_call.response.object_sets);
            }

            ROS_INFO("building travel time lookup tables");
            t0 = ros::WallTime::now();
            opc.prepareInitialTravelTimeLut(m_agent, robot_pose, cam_pose, m_world_frame_id);
            logtime("initial_tt_lut_time", t0);
            if(!m_log_dir.empty()) {
                fname.str("");
                fname.clear();
                fname << m_log_dir << "/initial-tt-map-" << iteration_counter << ".tab";
                opc.dumpInitialTravelTimeMap(fname.str());
            }

            size_t n_pruned = opc.pruneUnreachablePoses();
            logval("unreachable_poses_pruned", n_pruned);

            t0 = ros::WallTime::now();
            opc.prepareTravelTimeLut(m_agent, m_world_frame_id);
            logtime("mutual_tt_lut_time", t0);

            // HACK:
            // Find the number of discoverable cells as the nr. of cells seen by the greedy strategy and
            // assign equal probability to all cells with a sum of 1.
            // THIS IGNORES ALL PROBABILITIES IN THE REQUEST AND SHOULD BE REPLACED
            EqualProbabilityCellGain epcg;
            {
                epcg.p = 1.0 / n_cells;
                SearchPlan<EqualProbabilityCellGain> sp(epcg, opc);
                sp.greedy(9000);
                epcg.p = 1.0 / sp.detected_cells[sp.last_idx].size();

                // Termination criterion:
                if(sp.detected_cells[sp.last_idx].size() * cell_volume < goal.min_observable_volume)
                {
                    loginfo("termination criterion: min_observable_volume");
                    m_observe_volumes_server.setSucceeded();
                    return;
                }
            }

            ROS_INFO("entering planning phase");
            t0 = ros::WallTime::now();
            std::vector<size_t> plan;
            if(m_planning_mode == "simple")
            {
                plan = makePlanSimple(opc);
            }
            else if(m_planning_mode == "search")
            {
                SearchPlanner<EqualProbabilityCellGain> spl(epcg, opc);
                double etime;
                bool finished = spl.makePlan(m_depth_limit,
                                             m_max_rel_branch_cost,
                                             m_planning_timeout * 1000,
                                             plan, etime);
                if(!finished)
                {
                    logerror("planning timed out");
                }
            }
            else
            {
                ROS_ERROR_STREAM("unknown planning mode: " << m_planning_mode);
            }
            logtime("planning_time", t0);

            // termination criterion
            if(plan.size() < 2)
            {
                loginfo("termination criterion: no plan");
                m_observe_volumes_server.setSucceeded();
                return;
            }

            // log and publish some plan details
            SearchPlan<EqualProbabilityCellGain> sp(epcg, opc);
            sp.pushSequence(plan, 1, plan.size());
            sp.sendMarker(m_world_frame_id, m_marker_pub, "plan");
            if(!m_log_dir.empty()) {
                fname.str("");
                fname.clear();
                fname << m_log_dir << "/plan-timeplot-" << iteration_counter << ".tab";
                sp.writeTimeplot(fname.str());
            }

            size_t best_pose_idx = plan[1];
            logval("expected_move_time", opc.getInitialTravelTime(best_pose_idx));

            // move the robot
            ros::Time st0 = ros::Time::now();
            if(m_agent.achieveCamPose(opc.getPoses()[best_pose_idx].pose,
                                        opc.getPoses()[best_pose_idx].view_distance))
            {
                // wait for acquisition
                ros::Duration(m_agent.getAcquisitionTime()).sleep();
            }
            else
            {
                logerror("failed to achieve target pose");
            }
            logval("actual_move_time", (ros::Time::now() - st0).toSec());
        }
    }
};

#endif // OBJECT_SEARCH_MANAGER_H
