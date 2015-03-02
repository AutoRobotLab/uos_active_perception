#include "next_best_view_node.h"

#include "active_perception_map.h"
#include "octree_regions.h"
#include "observation_pose_sampler.h"

#include <ros/ros.h>
#include <pcl/ros/conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <octomap_ros/conversions.h>
#include <octomap/octomap.h>
#include <visualization_msgs/Marker.h>
#include <boost/random.hpp>
#include <race_next_best_view/ConditionalVisibilityMap.h>
#include <race_next_best_view/CellIds.h>

#include <ctime>
#include <list>
#include <memory>

NextBestViewNode::NextBestViewNode() :
    m_node_handle("~"),
    m_node_handle_pub(),
    m_point_cloud_subscriber(m_node_handle_pub.subscribe(
                                 "cloud_in",
                                 1,
                                 &NextBestViewNode::pointCloudCb,
                                 this)),
    m_get_bbox_percent_unseen_server(m_node_handle_pub.advertiseService(
                                         "/get_bbox_occupancy",
                                         &NextBestViewNode::getBboxOccupancyCb,
                                         this)),
    m_get_observation_camera_poses_server(m_node_handle_pub.advertiseService(
                                              "/get_observation_camera_poses",
                                              &NextBestViewNode::getObservationCameraPosesCb,
                                              this)),
    m_get_objects_to_remove_server(m_node_handle_pub.advertiseService(
                                       "/get_objects_to_remove",
                                       &NextBestViewNode::getObjectsToRemoveCb,
                                       this)),
    m_reset_volumes_server(m_node_handle_pub.advertiseService(
                                       "/reset_volumes",
                                       &NextBestViewNode::resetVolumesCb,
                                       this)),
    m_tf_listener(),
    m_marker_pub(m_node_handle_pub.advertise<visualization_msgs::Marker>("/next_best_view_marker", 10000)),
    m_perception_map(0.01)
{
    m_node_handle.param("resolution"    , m_resolution    , 0.05);
    m_node_handle.param("ray_skip"      , m_ray_skip      , 1.00);
    m_node_handle.param("world_frame_id", m_world_frame_id, std::string("/odom_combined"));

    // Set camera constraints from parameters (Defaults: xtion on calvin)
    m_node_handle.param("camera_frame_id",m_camera_constraints.frame_id  , std::string("/head_mount_kinect"));
    m_node_handle.param("height_min"    , m_camera_constraints.height_min, 1.5875);
    m_node_handle.param("height_max"    , m_camera_constraints.height_max, 1.5875);
    m_node_handle.param("pitch_min"     , m_camera_constraints.pitch_min , -0.935815); // [-53.6182 deg]
    m_node_handle.param("pitch_max"     , m_camera_constraints.pitch_max , -0.935815); // [-53.6182 deg]
    m_node_handle.param("range_min"     , m_camera_constraints.range_min , 0.4);
    m_node_handle.param("range_max"     , m_camera_constraints.range_max , 3.0);
    m_node_handle.param("hfov"          , m_camera_constraints.hfov      , 1.01229097);
    m_node_handle.param("vfov"          , m_camera_constraints.vfov      , 0.785398163);
    m_node_handle.param("roll"          , m_camera_constraints.roll      , PI);

    m_perception_map.setResolution(m_resolution);
}

double NextBestViewNode::getIntersectionVolume(
        const octomath::Vector3 &box1min,
        const octomath::Vector3 &box1max,
        const octomath::Vector3 &box2min,
        const octomath::Vector3 &box2max
){
    octomath::Vector3 max, min;
    // intersection min is the maximum of the two boxes' mins
    // intersection max is the minimum of the two boxes' maxes
    max.x() = std::min(box1max.x(), box2max.x());
    max.y() = std::min(box1max.y(), box2max.y());
    max.z() = std::min(box1max.z(), box2max.z());
    min.x() = std::max(box1min.x(), box2min.x());
    min.y() = std::max(box1min.y(), box2min.y());
    min.z() = std::max(box1min.z(), box2min.z());
    // make sure that max is actually larger in each dimension
    if(max.x() < min.x()) return 0.0;
    if(max.y() < min.y()) return 0.0;
    if(max.z() < min.z()) return 0.0;
    return (max.x() - min.x()) * (max.y() - min.y()) * (max.z() - min.z());
}

std::vector<tf::Vector3> NextBestViewNode::bboxVertices(race_msgs::BoundingBox const & bbox)
{
    // Construct a vector of bounding box vertices relative to the bounding box pose
    std::vector<tf::Vector3> bbox_points(8);
    bbox_points[0] = tf::Vector3(-bbox.dimensions.x/2, -bbox.dimensions.y/2, +bbox.dimensions.z/2);
    bbox_points[1] = tf::Vector3(-bbox.dimensions.x/2, -bbox.dimensions.y/2, -bbox.dimensions.z/2);
    bbox_points[2] = tf::Vector3(-bbox.dimensions.x/2, +bbox.dimensions.y/2, +bbox.dimensions.z/2);
    bbox_points[3] = tf::Vector3(-bbox.dimensions.x/2, +bbox.dimensions.y/2, -bbox.dimensions.z/2);
    bbox_points[4] = tf::Vector3(+bbox.dimensions.x/2, -bbox.dimensions.y/2, +bbox.dimensions.z/2);
    bbox_points[5] = tf::Vector3(+bbox.dimensions.x/2, -bbox.dimensions.y/2, -bbox.dimensions.z/2);
    bbox_points[6] = tf::Vector3(+bbox.dimensions.x/2, +bbox.dimensions.y/2, +bbox.dimensions.z/2);
    bbox_points[7] = tf::Vector3(+bbox.dimensions.x/2, +bbox.dimensions.y/2, -bbox.dimensions.z/2);
    // Transform points to bbox frame
    tf::Quaternion tf_orientation;
    tf::Point tf_position;
    tf::quaternionMsgToTF(bbox.pose_stamped.pose.orientation, tf_orientation);
    tf::pointMsgToTF(bbox.pose_stamped.pose.position, tf_position);
    tf::Transform transform(tf_orientation, tf_position);
    for(std::vector<tf::Vector3>::iterator it = bbox_points.begin(); it != bbox_points.end(); ++it)
    {
        *it = transform(*it);
    }
    return bbox_points;
}

bool NextBestViewNode::getAxisAlignedBounds(
        race_msgs::BoundingBox const & bbox,
        octomath::Vector3 & min,
        octomath::Vector3 & max
) const
{
    float inf = std::numeric_limits<float>::infinity();
    min.x() =  inf; min.y() =  inf; min.z() =  inf;
    max.x() = -inf; max.y() = -inf; max.z() = -inf;

    std::vector<tf::Vector3> bbox_vertices = bboxVertices(bbox);
    if(!m_tf_listener.waitForTransform(m_world_frame_id,
                                       bbox.pose_stamped.header.frame_id,
                                       bbox.pose_stamped.header.stamp,
                                       ros::Duration(1)))
    {
        ROS_ERROR_STREAM("next_best_view_node: Timed out while waiting for transform from " <<
                          bbox.pose_stamped.header.frame_id << " to " <<
                          m_world_frame_id);
        return false;
    }
    for(std::vector<tf::Vector3>::iterator it = bbox_vertices.begin(); it != bbox_vertices.end(); ++it)
    {
        geometry_msgs::PointStamped pin, pout;
        pin.header = bbox.pose_stamped.header;
        pin.point.x = it->x();
        pin.point.y = it->y();
        pin.point.z = it->z();
        m_tf_listener.transformPoint(m_world_frame_id, pin, pout);
        if(pout.point.x < min.x()) min.x() = pout.point.x;
        if(pout.point.y < min.y()) min.y() = pout.point.y;
        if(pout.point.z < min.z()) min.z() = pout.point.z;
        if(pout.point.x > max.x()) max.x() = pout.point.x;
        if(pout.point.y > max.y()) max.y() = pout.point.y;
        if(pout.point.z > max.z()) max.z() = pout.point.z;
    }
    return true;
}

OcTreeBoxSet NextBestViewNode::boxSetFromMsg(std::vector<race_msgs::BoundingBox> const & bbox_vec) const
{
    OcTreeBoxSet boxSet;
    for(unsigned int i = 0; i < bbox_vec.size(); i++)
    {
        octomath::Vector3 min, max;
        if(getAxisAlignedBounds(bbox_vec[i], min, max))
        {
            octomap::OcTreeKey mink, maxk;
            if(m_perception_map.getOccupancyMap().coordToKeyChecked(min, mink) &&
               m_perception_map.getOccupancyMap().coordToKeyChecked(max, maxk))
            {
                boxSet.elements.push_back(OcTreeBbox(mink, maxk));
            }
        }
    }
    return boxSet;
}

bool NextBestViewNode::getBboxOccupancyCb(race_next_best_view::GetBboxOccupancy::Request &req,
                                          race_next_best_view::GetBboxOccupancy::Response &res)
{
    boost::mutex::scoped_lock lock(m_map_mutex);
    octomath::Vector3 min, max;
    getAxisAlignedBounds(req.bbox, min, max);
    double total_volume = (max.x() - min.x()) * (max.y() - min.y()) * (max.z() - min.z());
    double free_volume = 0.0, occupied_volume = 0.0;
    for(octomap::OcTree::leaf_bbx_iterator it = m_perception_map.getOccupancyMap().begin_leafs_bbx(min,max),
        end = m_perception_map.getOccupancyMap().end_leafs_bbx();
        it != end; ++it)
    {
        double side_len_half = it.getSize() / 2.0;
        octomath::Vector3 vox_min = it.getCoordinate(), vox_max = it.getCoordinate();
        vox_min.x() -= side_len_half;
        vox_min.y() -= side_len_half;
        vox_min.z() -= side_len_half;
        vox_max.x() += side_len_half;
        vox_max.y() += side_len_half;
        vox_max.z() += side_len_half;
        double v = getIntersectionVolume(min, max, vox_min, vox_max);
        if(m_perception_map.getOccupancyMap().isNodeOccupied(*it))
        {
            occupied_volume += v;
        }
        else
        {
            free_volume += v;
        }
    }
    res.free = free_volume / total_volume;
    res.occupied = occupied_volume / total_volume;
    res.unknown = 1.0 - res.free - res.occupied;
    return true;
}

bool NextBestViewNode::getObservationCameraPosesCb(race_next_best_view::GetObservationCameraPoses::Request& req,
                                                   race_next_best_view::GetObservationCameraPoses::Response& res)
{
    ros::Time callback_timeout = ros::Time::now() + req.timeout;
    ObservationPoseSampler ops(m_camera_constraints);
    boost::mt19937 rng(std::time(0));
    boost::uniform_01<> rand_u01;
    // TODO: Shrink the ROI to an area that is in principle observable
    // (not higher/lower than the camera constraints allow observations to be made)
    // Should be implemented together with the ObservationSampler class which will wrap the
    // sampleObservationSpace method and allow to ask for single samples

    // translate roi to octomap
    OcTreeBoxSet roi = boxSetFromMsg(req.roi);
    unsigned int roi_cell_count = roi.cellCount();
    if(!roi_cell_count)
    {
        // If the roi is empty (exploration mode), we estimate the number of cells in the view cone
        roi_cell_count =
        ((std::sqrt((2.0 * std::pow(m_camera_constraints.range_max, 2)) * (1.0 - std::cos(m_camera_constraints.hfov))) *
          std::sqrt((2.0 * std::pow(m_camera_constraints.range_max, 2)) * (1.0 - std::cos(m_camera_constraints.vfov))) *
          m_camera_constraints.range_max) / 3.0) / std::pow(m_resolution, 3);
    }
    // translate objects to octomap
    OcTreeBoxSet object_boxes = boxSetFromMsg(req.objects);
    ActivePerceptionMap::ObjectSetMap object_sets;
    object_sets.rehash(object_boxes.elements.size() / object_sets.max_load_factor());
    ROS_INFO_STREAM("NBV sampling with " << object_boxes.elements.size() << " objects.");

    // make local copy of the map, so that sampling and mapping can occur in parallel
    std::auto_ptr<ActivePerceptionMap> map;
    {
        boost::mutex::scoped_lock lock(m_map_mutex);
        map.reset(new ActivePerceptionMap(m_perception_map));
    }

    // Some derived camera parameters
    double azimuth_min = -m_camera_constraints.hfov / 2.0;
    double azimuth_max =  m_camera_constraints.hfov / 2.0;
    double inclination_min = -m_camera_constraints.vfov / 2.0;
    double inclination_max =  m_camera_constraints.vfov / 2.0;
    // Find the right discretization of ray angles so that each octree voxel at max range is hit by one ray.
    double angle_increment =
            std::acos(1 - (std::pow(m_resolution, 2) / (2.0 * std::pow(m_camera_constraints.range_max, 2))));

    bool observation_position_set = !req.observation_position.header.frame_id.empty();
    std::vector<octomap::point3d> fringe_centers;
    octomath::Vector3 observation_position;
    if(!observation_position_set)
    {
        // Gather unknown voxel centers
        // TODO: This whole method of copying voxel centers into a vector is rather costly for many fringe voxels.
        //       The sampling procedure could be reformulated in such a way that we only need to iterate once through
        //       all the fringe voxels in the octree.
        //       [Not really valid anymore, because fringe voxels are additionally generated for roi boundaries.
        //        Also, this does not seem to be a bottleneck.]
        if(req.roi.empty())
        {
            // If the requested roi is empty, use all fringe voxels
            fringe_centers = map->getFringeCenters();
        }
        for(unsigned int i_roi = 0; i_roi < roi.elements.size(); i_roi++)
        {
            OcTreeBbox box = roi.elements[i_roi];
            octomath::Vector3 min = map->getFringeMap().keyToCoord(box.min);
            octomath::Vector3 max = map->getFringeMap().keyToCoord(box.max);
            std::vector<octomap::point3d> roi_fringe_centers;
            // add fringe voxels within the roi
            roi_fringe_centers = map->getFringeCenters(min, max);
            fringe_centers.insert(fringe_centers.end(), roi_fringe_centers.begin(), roi_fringe_centers.end());
            // generate fringe voxels at roi boundary
            roi_fringe_centers = map->genBoundaryFringeCenters(min, max);
            fringe_centers.insert(fringe_centers.end(), roi_fringe_centers.begin(), roi_fringe_centers.end());
        }
        if(fringe_centers.empty()) return true;
        // publish a marker for active fringe voxels
        {
            visualization_msgs::Marker marker;
            marker.action = visualization_msgs::Marker::ADD;
            marker.type = visualization_msgs::Marker::CUBE_LIST;
            marker.lifetime = ros::Duration();
            marker.scale.x = m_resolution;
            marker.scale.y = m_resolution;
            marker.scale.z = m_resolution;
            marker.color.r = 1.0;
            marker.color.g = 1.0;
            marker.color.b = 0.0;
            marker.color.a = 1.0;
            for(unsigned int i = 0; i < fringe_centers.size(); i++)
            {
                marker.points.push_back(octomap::pointOctomapToMsg(fringe_centers[i]));
            }
            marker.ns = "active_fringe";
            marker.id = 0;
            marker.header.frame_id = m_world_frame_id;
            marker.header.stamp = ros::Time::now();
            m_marker_pub.publish(marker);
        }
    }
    else
    {
        if(!m_tf_listener.waitForTransform(m_world_frame_id,
                                           req.observation_position.header.frame_id,
                                           req.observation_position.header.stamp,
                                           ros::Duration(1)))
        {
            ROS_ERROR_STREAM("next_best_view_node: Timed out while waiting for transform from " <<
                              req.observation_position.header.frame_id << " to " <<
                              m_world_frame_id);
            return false;
        }
        geometry_msgs::PointStamped obspos_msg;
        m_tf_listener.transformPoint(m_world_frame_id, req.observation_position, obspos_msg);
        observation_position = octomap::pointMsgToOctomap(obspos_msg.point);
    }

    for(int sample_id = 0; sample_id < req.sample_size; ++sample_id)
    {
        // Abort if there is no time left
        if(req.timeout > ros::Duration(0,0) && ros::Time::now() > callback_timeout) {
            break;
        }

        tf::Transform sample;
        if(observation_position_set)
        {
            sample = ops.genPoseSample(observation_position, req.lock_height);
        }
        else
        {
            try
            {
                sample = ops.genObservationSample(
                            fringe_centers[boost::uniform_int<>(0, fringe_centers.size() - 1)(rng)]);
            }
            catch (std::runtime_error e)
            {
                ROS_WARN("Skipped unobservable fringe voxel");
            }

        }

        octomap::point3d cam_point = octomap::pointTfToOctomap(sample.getOrigin());

        ActivePerceptionMap::OcTreeKeyMap discovery_field;
        discovery_field.rehash(roi_cell_count / discovery_field.max_load_factor()); // reserve enough space

        for(double azimuth = azimuth_min; azimuth <= azimuth_max; azimuth += angle_increment)
        {
            for(double inclination = inclination_min; inclination <= inclination_max; inclination += angle_increment)
            {
                if(rand_u01(rng) < req.ray_skip)
                {
                    continue;
                }
                tf::Vector3 ray_end_in_cam(m_camera_constraints.range_max * std::cos(azimuth),
                                           m_camera_constraints.range_max * std::sin(azimuth),
                                           m_camera_constraints.range_max * std::sin(inclination));
                octomap::point3d ray_end = octomap::pointTfToOctomap(sample(ray_end_in_cam));
                map->estimateRayGainObjectAware(cam_point, ray_end, roi, object_boxes, object_sets, discovery_field);
            }
        }
        unsigned int gain = discovery_field.size();

        // Write pose candidate to answer
        geometry_msgs::Pose camera_pose_msg;
        tf::poseTFToMsg(sample, camera_pose_msg);
        if(gain > 0)
        {
            res.camera_poses.push_back(camera_pose_msg);
            res.information_gains.push_back(gain * std::pow(m_resolution, 3));
            // Prepare the conditional visibility map for this sample
            boost::unordered_map<unsigned int, std::list<unsigned long> > cvm_hashed;
            for(ActivePerceptionMap::OcTreeKeyMap::iterator it = discovery_field.begin();
                it != discovery_field.end();
                ++it)
            {
                // convert OcTreeKey to single long id and insert it into the hashed cvm
                unsigned long cell_id = it->first[0];
                cell_id = cell_id << 8;
                cell_id += it->first[1];
                cell_id = cell_id << 8;
                cell_id += it->first[1];
                cvm_hashed[it->second].push_back(cell_id);
            }
            // Now build a cvm_msg from the hashed cvm
            // TODO: These types and conversion functions should probably get their own header
            race_next_best_view::ConditionalVisibilityMap cvm_msg;
            for(boost::unordered_map<unsigned int, std::list<unsigned long> >::iterator it = cvm_hashed.begin();
                it != cvm_hashed.end();
                ++it)
            {
                cvm_msg.object_set_ids.push_back(it->first);
                race_next_best_view::CellIds cell_ids_msg;
                cell_ids_msg.cell_ids.reserve(it->second.size());
                cell_ids_msg.cell_ids.insert(cell_ids_msg.cell_ids.begin(), it->second.begin(), it->second.end());
                cvm_msg.cell_id_sets.push_back(cell_ids_msg);
            }
            res.cvms.push_back(cvm_msg);
        }

        // Send a marker
        visualization_msgs::Marker marker;
        marker.action = visualization_msgs::Marker::ADD;
        marker.type = visualization_msgs::Marker::ARROW;
        marker.lifetime = ros::Duration(10);
        marker.scale.x = 1;
        marker.scale.y = 1;
        marker.scale.z = 0.2;
        if(gain > 0) {
            marker.color.g = ((double) gain) / roi_cell_count;
            marker.color.r = 1.0 - marker.color.g;
        } else {
            marker.color.b = 1.0;
        }
        marker.color.a = 0.5;
        marker.pose = camera_pose_msg;
        marker.header.frame_id = m_world_frame_id;
        marker.header.stamp = ros::Time::now();
        marker.id = sample_id;
        marker.ns = "nbv_samples";
        m_marker_pub.publish(marker);
    }

    // Write object set ids
    res.object_sets.resize(object_sets.size());
    for(ActivePerceptionMap::ObjectSetMap::iterator it = object_sets.begin(); it != object_sets.end(); ++it)
    {
        res.object_sets[it->second].objects.reserve(it->first.size());
        res.object_sets[it->second].objects.insert(
                    res.object_sets[it->second].objects.begin(), it->first.begin(), it->first.end());
    }

    return true;
}

bool NextBestViewNode::getObjectsToRemoveCb(race_next_best_view::GetObjectsToRemove::Request& req,
                                            race_next_best_view::GetObjectsToRemove::Response& res)
{
    // TODO: Implement
    return false;
}

void NextBestViewNode::pointCloudCb(sensor_msgs::PointCloud2 const & cloud)
{
    // Find sensor origin
    tf::StampedTransform sensor_to_world_tf, camera_to_world_tf;
    try
    {
        m_tf_listener.waitForTransform(m_world_frame_id, cloud.header.frame_id, cloud.header.stamp, ros::Duration(2.0));
        m_tf_listener.lookupTransform(m_world_frame_id, cloud.header.frame_id, cloud.header.stamp, sensor_to_world_tf);
        m_tf_listener.lookupTransform(m_world_frame_id, m_camera_constraints.frame_id, cloud.header.stamp, camera_to_world_tf);
    }
    catch(tf::TransformException& ex)
    {
        ROS_ERROR_STREAM("next_best_view_node: Transform error of sensor data: "
                         << ex.what()
                         << ", cannot integrate data.");
        return;
    }

    // Ugly converter cascade to get octomap_cloud
    pcl::PointCloud<pcl::PointXYZ> pcl_cloud;
    octomap::Pointcloud octomap_cloud;
    pcl::fromROSMsg(cloud, pcl_cloud);
    octomap::pointcloudPCLToOctomap(pcl_cloud, octomap_cloud);

    // Integrate point cloud
    {
        boost::mutex::scoped_lock lock(m_map_mutex);
        m_perception_map.integratePointCloud(octomap_cloud,
                                             octomap::poseTfToOctomap(sensor_to_world_tf),
                                             camera_to_world_tf,
                                             m_camera_constraints);
    }

    // Publish rviz map visualization
    visualization_msgs::Marker marker = m_perception_map.genOccupancyMarker();
    marker.header.frame_id = m_world_frame_id;
    marker.header.stamp = ros::Time::now();
    marker.id = 0;
    marker.ns = "occupancy_map";
    m_marker_pub.publish(marker);
    marker = m_perception_map.genFringeMarker();
    marker.header.frame_id = m_world_frame_id;
    marker.header.stamp = ros::Time::now();
    marker.id = 0;
    marker.ns = "fringe_map";
    m_marker_pub.publish(marker);
}

bool NextBestViewNode::resetVolumesCb
(
        race_next_best_view::ResetVolumes::Request & req,
        race_next_best_view::ResetVolumes::Response & resp)
{
    boost::mutex::scoped_lock lock(m_map_mutex);
    bool success = true;
    for(std::vector<race_msgs::BoundingBox>::iterator it = req.volumes.begin();
        it < req.volumes.end();
        ++it)
    {
        octomath::Vector3 min, max;
        if(getAxisAlignedBounds(*it, min, max))
        {
            m_perception_map.resetVolume(min, max);
        }
        else
        {
            success = false;
        }
    }
    return success;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "next_best_view_node");
    NextBestViewNode node;
    ROS_INFO("next_best_view_node: Initialized!");
    ros::MultiThreadedSpinner(4).spin();
}
