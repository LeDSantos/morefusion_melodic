// Copyright (c) 2019 Kentaro Wada

#include "morefusion_ros/OctomapServer.h"

using octomap_msgs::Octomap;

namespace morefusion_ros {

OctomapServer::OctomapServer() {
  nh_ = ros::NodeHandle();
  pnh_ = ros::NodeHandle("~");

  instance_counter_ = 0;
  tree_depth_ = 16;
  tree_depth_max_ = 16;
  reset_stamp_ = ros::Time::now();

  // parameters for mapping
  pnh_.param("resolution", resolution_, 0.05);
  pnh_.param("sensor_model/max_range", max_range_, -1.0);
  pnh_.param("sensor_model/hit", probability_hit_, 0.7);
  pnh_.param("sensor_model/miss", probability_miss_, 0.4);
  pnh_.param("sensor_model/min", probability_min_, 0.12);
  pnh_.param("sensor_model/max", probability_max_, 0.97);
  pnh_.param("compress_map", do_compress_map_, false);
  pnh_.param("use_render_service", use_render_service_, false);

  // paramters for publishing
  pnh_.param("frame_id", frame_id_world_, std::string("map"));
  pnh_.param("sensor_frame_id", frame_id_sensor_, std::string("camera_color_optical_frame"));
  pnh_.param("filter_speckles", do_filter_speckles_, false);

  tf_listener_ = new tf::TransformListener(ros::Duration(30));

  pub_binary_map_ = pnh_.advertise<Octomap>("output/octomap_binary", 1);
  pub_full_map_ = pnh_.advertise<Octomap>("output/octomap_full", 1);
  pub_grids_ = pnh_.advertise<morefusion_ros::VoxelGridArray>("output/grids", 1);
  pub_grids_noentry_ = pnh_.advertise<morefusion_ros::VoxelGridArray>(
    "output/grids_noentry", 1);
  pub_markers_free_ = pnh_.advertise<visualization_msgs::MarkerArray>("output/markers_free", 1);
  pub_markers_bg_ = pnh_.advertise<visualization_msgs::MarkerArray>("output/markers_bg", 1);
  pub_markers_fg_ = pnh_.advertise<visualization_msgs::MarkerArray>("output/markers_fg", 1);
  pub_label_rendered_ = pnh_.advertise<sensor_msgs::Image>("output/label_rendered", 1);
  pub_label_tracked_ = pnh_.advertise<sensor_msgs::Image>("output/label_tracked", 1);
  pub_class_ = pnh_.advertise<morefusion_ros::ObjectClassArray>("output/class", 1);

  sub_camera_ = new message_filters::Subscriber<sensor_msgs::CameraInfo>(
    pnh_, "input/camera_info", 5);
  sub_depth_ = new message_filters::Subscriber<sensor_msgs::Image>(
    pnh_, "input/depth", 5);
  sub_pcd_ = new message_filters::Subscriber<sensor_msgs::PointCloud2>(
    pnh_, "input/points", 5);
  sub_label_ins_ = new message_filters::Subscriber<sensor_msgs::Image>(
    pnh_, "input/label_ins", 5);
  sub_class_ = new message_filters::Subscriber<morefusion_ros::ObjectClassArray>(
    pnh_, "input/class", 5);
  sync_ = new message_filters::Synchronizer<ExactSyncPolicy>(100);
  sync_->connectInput(*sub_camera_, *sub_depth_, *sub_pcd_, *sub_label_ins_, *sub_class_);
  sync_->registerCallback(
    boost::bind(&OctomapServer::insertCloudCallback, this, _1, _2, _3, _4, _5));

  client_render_ = pnh_.serviceClient<morefusion_ros::RenderVoxelGridArray>("render");

  server_reset_ = pnh_.advertiseService("reset", &OctomapServer::resetCallback, this);

  dynamic_reconfigure::Server<morefusion_ros::OctomapServerConfig>::CallbackType f =
    boost::bind(&OctomapServer::configCallback, this, _1, _2);
  server_reconfig_.setCallback(f);

  ROS_INFO_BLUE("Initialized");
}

bool OctomapServer::resetCallback(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res) {
  boost::mutex::scoped_lock lock(mutex_);
  octrees_.clear();
  class_ids_.clear();
  centers_.clear();
  instance_counter_ = 0;
  reset_stamp_ = ros::Time::now();
  return true;
}

void OctomapServer::configCallback(
  const morefusion_ros::OctomapServerConfig& config, const uint32_t level) {
  boost::mutex::scoped_lock lock(mutex_);
  ROS_INFO_BLUE("configCallback");
  m_groundAsNoEntry = config.ground_as_noentry;
  m_freeAsNoEntry = config.free_as_noentry;
}

void OctomapServer::insertCloudCallback(
    const sensor_msgs::CameraInfoConstPtr& camera_info_msg,
    const sensor_msgs::ImageConstPtr& depth_msg,
    const sensor_msgs::PointCloud2ConstPtr& cloud,
    const sensor_msgs::ImageConstPtr& ins_msg,
    const morefusion_ros::ObjectClassArrayConstPtr& class_msg) {
  boost::mutex::scoped_lock lock(mutex_);
  if (camera_info_msg->header.stamp < reset_stamp_) {
    return;
  }
  // Get TF
  tf::StampedTransform sensorToWorldTf;
  if (!tf_listener_->waitForTransform(frame_id_world_,
                                      cloud->header.frame_id,
                                      cloud->header.stamp,
                                      ros::Duration(0.1))) {
    return;
  }
  tf_listener_->lookupTransform(
    frame_id_world_, cloud->header.frame_id, cloud->header.stamp, sensorToWorldTf);
  Eigen::Matrix4f sensorToWorld;
  pcl_ros::transformAsMatrix(sensorToWorldTf, sensorToWorld);

  // ROSMsg -> PCL
  PCLPointCloud pc;
  pcl::fromROSMsg(*cloud, pc);

  // Transform pointcloud: sensor -> world (map)
  pcl::transformPointCloud(pc, pc, sensorToWorld);

  // ROSMsg -> OpenCV
  cv::Mat label_ins = cv_bridge::toCvCopy(ins_msg, ins_msg->encoding)->image;

  // Render
  cv::Mat label_ins_rend;
  if (use_render_service_) {
    morefusion_ros::RenderVoxelGridArray srv;
    tf::transformStampedTFToMsg(sensorToWorldTf, srv.request.transform);
    srv.request.camera_info = *camera_info_msg;
    srv.request.depth = *depth_msg;
    getGridsInWorldFrame(camera_info_msg->header.stamp, srv.request.grids);
    client_render_.call(srv);
    sensor_msgs::Image ins_rendered_msg = srv.response.label_ins;
    label_ins_rend = cv_bridge::toCvCopy(
      srv.response.label_ins, srv.response.label_ins.encoding)->image;
  } else {
    label_ins_rend = label_ins.clone();
    render(camera_info_msg, sensorToWorldTf.getOrigin(), pc, label_ins_rend, sensorToWorld);
  }
  // Publish Rendered Instance Label
  pub_label_rendered_.publish(
    cv_bridge::CvImage(ins_msg->header, "32SC1", label_ins_rend).toImageMsg());

  // Track Instance IDs
  std::map<int, unsigned> instance_id_to_class_id;
  for (size_t i = 0; i < class_msg->classes.size(); i++) {
    instance_id_to_class_id.insert(
      std::make_pair(
        class_msg->classes[i].instance_id,
        class_msg->classes[i].class_id));
  }
  morefusion_ros::utils::track_instance_id(
    /*reference=*/label_ins_rend,
    /*target=*/&label_ins,
    /*instance_id_to_class_id=*/&instance_id_to_class_id,
    /*instance_counter=*/&instance_counter_);
  for (std::map<int, unsigned>::iterator it = class_ids_.begin();
       it != class_ids_.end(); it++) {
    if (instance_id_to_class_id.find(it->first) == instance_id_to_class_id.end()) {
      instance_id_to_class_id.insert(std::make_pair(it->first, it->second));
    }
  }
  // Publish Tracked Instance Label
  pub_label_tracked_.publish(
    cv_bridge::CvImage(ins_msg->header, "32SC1", label_ins).toImageMsg());

  morefusion_ros::ObjectClassArray cls_rend_msg;
  cls_rend_msg.header = cloud->header;
  for (std::map<int, unsigned>::iterator it = class_ids_.begin();
       it != class_ids_.end(); it++) {
    if (it->first == -1) {
      continue;
    }
    morefusion_ros::ObjectClass cls;
    cls.instance_id = it->first;
    cls.class_id = it->second;
    cls.confidence = 1;
    cls_rend_msg.classes.push_back(cls);
  }
  pub_class_.publish(cls_rend_msg);

  // Update Map
  insertScan(sensorToWorldTf.getOrigin(), pc, label_ins, instance_id_to_class_id);

  // Publish Object Grids
  std::set<int> instance_ids_active = morefusion_ros::utils::unique<int>(label_ins_rend);
  publishGrids(cloud->header.stamp, sensorToWorld, instance_ids_active);

  // Publish Map
  publishAll(cloud->header.stamp);
}

void OctomapServer::render(
    const sensor_msgs::CameraInfoConstPtr& camera_info_msg,
    const tf::Point& sensorOriginTf,
    const PCLPointCloud& pc,
    cv::Mat& label_ins_rend,
    const Eigen::Matrix4f sensorToWorld) {
  octomap::point3d sensorOrigin = octomap::pointTfToOctomap(sensorOriginTf);
  std::vector<int> instance_ids = morefusion_ros::utils::keys(octrees_);
  cv::Mat depth = cv::Mat::zeros(pc.height, pc.width, CV_32FC1);
  depth.setTo(NAN);
  label_ins_rend.setTo(-2);
  #pragma omp parallel for
  for(int instance_id_index = 0; instance_id_index < instance_ids.size(); instance_id_index++){
    int instance_id = instance_ids[instance_id_index];
  ///for each(int instance_id in instance_ids) {
  ///fonte: https://www.w3schools.com/cpp/cpp_for_loop.asp e https://stackoverflow.com/questions/20531335/compilation-error-with-for-each-loop-in-c-vs2010
  ///for (int instance_id : instance_ids) {
    if (instance_id == -1) {
      // skip background objects
      continue;
    }
    OcTreeT* octree = octrees_.find(instance_id)->second;

    for (size_t index = 0 ; index < pc.points.size(); index++) {
      int width_index = index % pc.width;
      int height_index = index / pc.width;
      if (width_index % 2 != 0 || height_index % 2 != 0) {
        continue;
      }

      bool check_in_bbox;
      octomap::point3d point;
      if (std::isnan(pc.points[index].x) ||
          std::isnan(pc.points[index].y) ||
          std::isnan(pc.points[index].z)) {
        float fx = camera_info_msg->K[0];
        float fy = camera_info_msg->K[4];
        float cx = camera_info_msg->K[2];
        float cy = camera_info_msg->K[5];

        float z = 1;  // max depth
        float x = z * (width_index - cx) / fx;
        float y = z * (height_index - cy) / fy;
        PCLPointCloud pc2;
        pc2.push_back(PCLPoint(x, y, z));
        pcl::transformPointCloud(pc2, pc2, sensorToWorld);

        check_in_bbox = false;
        point = octomap::point3d(pc2[0].x, pc2[0].y, pc2[0].z);
      } else {
        check_in_bbox = true;
        point = octomap::point3d(pc.points[index].x, pc.points[index].y, pc.points[index].z);
      }

      octomap::point3d direction = point - sensorOrigin;

      if (check_in_bbox && !octree->inBBX(point)) {
        continue;
      }

      octomap::point3d end;
      bool hit = octree->castRay(/*origin=*/sensorOrigin, /*direction=*/direction, /*end=*/end, /*ignoreUnknownCells=*/true, /*maxRange=*/(point - sensorOrigin).norm() * 1.1);
      if (!hit) {
        continue;
      }

      octomap::point3d intersection;
#if 0
      octree->getRayIntersection(/*origin=*/sensorOrigin, /*direction=*/direction, /*center=*/end, /*intersection=*/intersection);
#else
      intersection = end;
#endif

      #pragma omp critical
      {
        float d_old = depth.at<float>(height_index, width_index);
        float d_new = (intersection - sensorOrigin).norm();
        if ((d_old != d_old) || (d_new < d_old)) {
          depth.at<float>(height_index, width_index) = d_new;
          for (int dj = -1; dj != 1; dj++) {
            int j = height_index + dj;
            for (int di = -1; di != 1; di++) {
              int i = width_index + di;
              if (j >= 0 && i >= 0 && j < label_ins_rend.rows && i < label_ins_rend.cols) {
                label_ins_rend.at<int32_t>(j, i) = instance_id;
              }
            }
          }
        }
      }
    }
  }
}

void OctomapServer::insertScan(
    const tf::Point& sensorOriginTf,
    const PCLPointCloud& pc,
    const cv::Mat& label_ins,
    const std::map<int, unsigned>& instance_id_to_class_id) {
  octomap::point3d sensorOrigin = octomap::pointTfToOctomap(sensorOriginTf);

  std::set<int> instance_ids = morefusion_ros::utils::unique<int>(label_ins);
  octomap::KeySet free_cells_bg;
  std::map<int, octomap::KeySet> occupied_cells;
  std::set<int> new_instance_ids;
  for (int instance_id : instance_ids) {
    if (instance_id == -2) {
      // -1: background, -2: uncertain (e.g., boundary)
      continue;
    }
    unsigned class_id = 0;
    double pitch = resolution_;
    if (instance_id >= 0) {
      if (instance_id_to_class_id.find(instance_id) == instance_id_to_class_id.end()) {
        ROS_FATAL("Can't find instance_id [%d] in instance_id_to_class_id", instance_id);
        return;
      } else {
        class_id = instance_id_to_class_id.find(instance_id)->second;
      }
      pitch = morefusion_ros::utils::class_id_to_voxel_pitch(class_id);
    }
    if (octrees_.find(instance_id) == octrees_.end()) {
      OcTreeT* octree = new OcTreeT(pitch);
      octree->setProbHit(probability_hit_);
      octree->setProbMiss(probability_miss_);
      octree->setClampingThresMin(probability_min_);
      octree->setClampingThresMax(probability_max_);
      octrees_.insert(std::make_pair(instance_id, octree));
      class_ids_.insert(std::make_pair(instance_id, class_id));
      new_instance_ids.insert(instance_id);
    }
    occupied_cells.insert(std::make_pair(instance_id, octomap::KeySet()));
  }
  assert(octrees_.find(-1) != octrees_.end());
  assert(occupied_cells.find(-1) != occupied_cells.end());

  // all other points: free on ray, occupied on endpoint:
  std::map<int, PCLPointCloud> instance_id_to_points;
  #pragma omp parallel for
  for (size_t index = 0 ; index < pc.points.size(); index++) {
    size_t width_index = index % pc.width;
    size_t height_index = index / pc.width;
    if (width_index % 2 != 0 || height_index % 2 != 0) {
      continue;
    }
    if (std::isnan(pc.points[index].x) ||
        std::isnan(pc.points[index].y) ||
        std::isnan(pc.points[index].z)) {
      continue;
    }

    octomap::point3d point(pc.points[index].x, pc.points[index].y, pc.points[index].z);
    int instance_id = label_ins.at<int32_t>(height_index, width_index);

    #pragma omp critical
    if (instance_id != -2) {
      if (instance_id_to_points.find(instance_id) == instance_id_to_points.end()) {
        instance_id_to_points.insert(std::make_pair(instance_id, PCLPointCloud()));
      } else {
        instance_id_to_points.find(instance_id)->second.push_back(pc.points[index]);
      }
    }

    // maxrange check
    if ((max_range_ < 0.0) || ((point - sensorOrigin).norm() <= max_range_)) {
      // free cells
      octomap::KeyRay key_ray;
      OcTreeT* octree_bg = octrees_.find(-1)->second;
      if (octree_bg->computeRayKeys(sensorOrigin, point, key_ray)) {
        #pragma omp critical
        free_cells_bg.insert(key_ray.begin(), key_ray.end());
      }
      // occupied endpoint
      octomap::OcTreeKey key;
      if (instance_id != -2) {
        if (octrees_.find(instance_id)->second->coordToKeyChecked(point, key)) {
          #pragma omp critical
          occupied_cells.find(instance_id)->second.insert(key);
        }
      }
      if (instance_id != -1) {
        if (octree_bg->coordToKeyChecked(point, key)) {
          #pragma omp critical
          free_cells_bg.insert(key);
        }
      }
    } else {  // ray longer than maxrange:;
      octomap::point3d new_end = sensorOrigin + (point - sensorOrigin).normalized() * max_range_;
      octomap::KeyRay key_ray;
      OcTreeT* octree_bg = octrees_.find(-1)->second;
      if (octree_bg->computeRayKeys(sensorOrigin, new_end, key_ray)) {
        #pragma omp critical
        free_cells_bg.insert(key_ray.begin(), key_ray.end());
      }
    }
  }

  OcTreeT* octree_bg = octrees_.find(-1)->second;
  octomap::KeySet occupied_cells_bg = occupied_cells.find(-1)->second;
  for (octomap::KeySet::iterator it = free_cells_bg.begin(); it != free_cells_bg.end(); it++) {
    if (occupied_cells_bg.find(*it) == occupied_cells_bg.end()) {
      octree_bg->updateNode(*it, false);
    }
  }

  for (std::map<int, octomap::KeySet>::iterator i = occupied_cells.begin();
       i != occupied_cells.end(); i++) {
    int instance_id = i->first;
    octomap::KeySet key_set_occupied = i->second;
    OcTreeT* octree = octrees_.find(instance_id)->second;
    for (octomap::KeySet::iterator j = key_set_occupied.begin(); j != key_set_occupied.end(); j++) {
      octree->updateNode(*j, true);
    }
  }

  for (std::map<int, PCLPointCloud>::iterator it = instance_id_to_points.begin();
       it != instance_id_to_points.end(); it++) {
    int instance_id = it->first;
    PCLPointCloud points = it->second;
    OcTreeT* octree = octrees_.find(instance_id)->second;

    PCLPoint min_pt, max_pt;
    pcl::getMinMax3D(points, min_pt, max_pt);

    float min_x, min_y, min_z;
    float max_x, max_y, max_z;
    if (new_instance_ids.find(instance_id) == new_instance_ids.end()) {
      // not new instance
      octomap::point3d min_bbx = octree->getBBXMin();
      octomap::point3d max_bbx = octree->getBBXMax();
      min_x = std::min(min_bbx.x(), min_pt.x);
      min_y = std::min(min_bbx.y(), min_pt.y);
      min_z = std::min(min_bbx.z(), min_pt.z);
      max_x = std::max(max_bbx.x(), max_pt.x);
      max_y = std::max(max_bbx.y(), max_pt.y);
      max_z = std::max(max_bbx.z(), max_pt.z);
    } else {
      min_x = min_pt.x;
      min_y = min_pt.y;
      min_z = min_pt.z;
      max_x = max_pt.x;
      max_y = max_pt.y;
      max_z = max_pt.z;
    }

    octomap::point3d bbx_min(min_x, min_y, min_z);
    octomap::point3d bbx_max(max_x, max_y, max_z);
    octree->setBBXMin(bbx_min);
    octree->setBBXMax(bbx_max);

#if 0
    centers_.insert(std::make_pair(instance_id, octree->getBBXCenter()));
#else
    Eigen::Matrix<float, 4, 1> centroid;
    pcl::compute3DCentroid<PCLPoint, float>(
      /*cloud=*/it->second, /*centroid=*/centroid);
    octomap::point3d center(centroid(0, 0), centroid(1, 0), centroid(2, 0));
    centers_.insert(std::make_pair(instance_id, center));
#endif
  }

  if (do_compress_map_) {
    for (std::map<int, OcTreeT*>::iterator it = octrees_.begin(); it != octrees_.end(); it++) {
      it->second->prune();
    }
  }
}

void OctomapServer::getGridsInWorldFrame(
    const ros::Time& rostime,
    morefusion_ros::VoxelGridArray& grids) {
  grids.header.frame_id = frame_id_world_;
  grids.header.stamp = rostime;
  for (std::map<int, OcTreeT*>::iterator it_octree = octrees_.begin();
       it_octree != octrees_.end(); it_octree++) {
    int instance_id = it_octree->first;
    OcTreeT* octree = it_octree->second;

    if (instance_id == -1) {
      continue;
    }
    unsigned class_id = class_ids_.find(instance_id)->second;
    double pitch = morefusion_ros::utils::class_id_to_voxel_pitch(class_id);

    // world frame
    octomap::point3d center = centers_.find(instance_id)->second;

    morefusion_ros::VoxelGrid grid;
    grid.pitch = pitch;
    grid.dims.x = 32;
    grid.dims.y = 32;
    grid.dims.z = 32;
    grid.origin.x = center.x() - (grid.dims.x / 2.0 - 0.5) * grid.pitch;
    grid.origin.y = center.y() - (grid.dims.y / 2.0 - 0.5) * grid.pitch;
    grid.origin.z = center.z() - (grid.dims.z / 2.0 - 0.5) * grid.pitch;
    grid.instance_id = instance_id;
    grid.class_id = class_id;

    for (size_t i = 0; i < grid.dims.x; i++) {
      for (size_t j = 0; j < grid.dims.y; j++) {
        for (size_t k = 0; k < grid.dims.z; k++) {
          double x, y, z;
          // in world
          x = grid.origin.x + grid.pitch * i;
          y = grid.origin.y + grid.pitch * j;
          z = grid.origin.z + grid.pitch * k;

          size_t index = i * grid.dims.y * grid.dims.z + j * grid.dims.z + k;

          octomap::OcTreeNode* node = octree->search(x, y, z, /*depth=*/0);
          if ((node != NULL) && (node->getOccupancy() > 0.5)) {
            grid.indices.push_back(index);
            grid.values.push_back(node->getOccupancy());
          }
        }
      }
    }
    grids.grids.push_back(grid);
  }
}

void OctomapServer::publishGrids(
    const ros::Time& rostime,
    const Eigen::Matrix4f& sensorToWorld,
    const std::set<int>& instance_ids_active) {
  if (octrees_.size() == 0) {
    return;
  }

  morefusion_ros::VoxelGridArray grids;
  grids.header.frame_id = frame_id_sensor_;
  grids.header.stamp = rostime;
  morefusion_ros::VoxelGridArray grids_noentry;
  grids_noentry.header = grids.header;
  for (std::map<int, OcTreeT*>::iterator it_octree = octrees_.begin();
       it_octree != octrees_.end(); it_octree++) {
    int instance_id = it_octree->first;
    if (instance_id == -1) {
      continue;
    }
    //if (instance_ids_active.find(instance_id) == instance_ids_active.end()) {
    //  // inactive
    //  continue;
    //}

    OcTreeT* octree = it_octree->second;
    unsigned class_id = class_ids_.find(instance_id)->second;
    double pitch = morefusion_ros::utils::class_id_to_voxel_pitch(class_id);

    octomap::point3d center = centers_.find(instance_id)->second;

    PCLPointCloud center_sensor;
    center_sensor.push_back(PCLPoint(center.x(), center.y(), center.z()));
    pcl::transformPointCloud(center_sensor, center_sensor, sensorToWorld.inverse());

    morefusion_ros::VoxelGrid grid;
    grid.pitch = pitch;
    grid.dims.x = 32;
    grid.dims.y = 32;
    grid.dims.z = 32;
    grid.origin.x = center_sensor.points[0].x - (grid.dims.x / 2.0 - 0.5) * grid.pitch;
    grid.origin.y = center_sensor.points[0].y - (grid.dims.y / 2.0 - 0.5) * grid.pitch;
    grid.origin.z = center_sensor.points[0].z - (grid.dims.z / 2.0 - 0.5) * grid.pitch;
    grid.instance_id = instance_id;
    grid.class_id = class_id;

    morefusion_ros::VoxelGrid grid_noentry;
    grid_noentry.pitch = grid.pitch;
    grid_noentry.dims = grid.dims;
    grid_noentry.origin = grid.origin;
    grid_noentry.instance_id = grid.instance_id;
    grid_noentry.class_id = grid.class_id;
    for (size_t i = 0; i < grid.dims.x; i++) {
      for (size_t j = 0; j < grid.dims.y; j++) {
        for (size_t k = 0; k < grid.dims.z; k++) {
          double x, y, z;
          // in sensor
          x = grid.origin.x + grid.pitch * i;
          y = grid.origin.y + grid.pitch * j;
          z = grid.origin.z + grid.pitch * k;

          // in world
          PCLPointCloud p_world;
          p_world.push_back(PCLPoint(x, y, z));  // sensor
          pcl::transformPointCloud(p_world, p_world, sensorToWorld);
          x = p_world.points[0].x;
          y = p_world.points[0].y;
          z = p_world.points[0].z;

          size_t index = i * grid.dims.y * grid.dims.z + j * grid.dims.z + k;
          if (m_groundAsNoEntry && (z < 0)) {
            grid_noentry.indices.push_back(index);
            grid_noentry.values.push_back(probability_max_);
            continue;
          }

          octomap::OcTreeNode* node = octree->search(x, y, z, /*depth=*/0);
          if ((node != NULL) && (node->getOccupancy() > 0.5)) {
            grid.indices.push_back(index);
            grid.values.push_back(node->getOccupancy());
          } else {
            for (std::map<int, OcTreeT*>::iterator it_octree_other = octrees_.begin();
                 it_octree_other != octrees_.end(); it_octree_other++) {
              if (it_octree_other->first == instance_id) {
                continue;
              }
              OcTreeT* octree_other = it_octree_other->second;
              node = octree_other->search(x, y, z, /*depth=*/0);
              if (node != NULL) {
                double occupancy = node->getOccupancy();
                if ((it_octree_other->first == -1) &&
                    m_freeAsNoEntry && (occupancy < 0.5)) {
                  grid_noentry.indices.push_back(index);
                  grid_noentry.values.push_back(1 - occupancy);
                } else if (occupancy >= probability_max_) {
                  grid_noentry.indices.push_back(index);
                  grid_noentry.values.push_back(occupancy);
                }
              }
            }
          }
        }
      }
    }
    grids.grids.push_back(grid);
    grids_noentry.grids.push_back(grid_noentry);
  }
  pub_grids_.publish(grids);
  pub_grids_noentry_.publish(grids_noentry);
}

void OctomapServer::publishAll(const ros::Time& rostime) {
  if (octrees_.size() == 0) {
    return;
  }

  bool publishFreeMarkerArray = pub_markers_free_.getNumSubscribers() > 0;
  bool publishMarkerArray = pub_markers_bg_.getNumSubscribers() > 0 ||
                            pub_markers_fg_.getNumSubscribers() > 0;
  bool publishBinaryMap = pub_binary_map_.getNumSubscribers() > 0;
  bool publishFullMap = pub_full_map_.getNumSubscribers() > 0;

  // init markers for free space:
  visualization_msgs::MarkerArray freeNodesVis;
  // each array stores all cubes of a different size, one for each depth level:
  freeNodesVis.markers.resize(tree_depth_ + 1);

  geometry_msgs::Pose pose;
  pose.orientation = tf::createQuaternionMsgFromYaw(0.0);

  // now, traverse all leafs in the tree:
  std::map<int, visualization_msgs::MarkerArray> occupiedNodesVisAll;
  for (std::map<int, OcTreeT*>::iterator it_octree = octrees_.begin();
       it_octree != octrees_.end(); it_octree++) {
    // init markers:
    visualization_msgs::MarkerArray occupiedNodesVis;
    // each array stores all cubes of a different size, one for each depth level:
    occupiedNodesVis.markers.resize(tree_depth_ + 1);

    const int instance_id = it_octree->first;
    OcTreeT* octree = it_octree->second;
    for (OcTreeT::iterator it = octree->begin(tree_depth_max_);
         it != octree->end(); it++) {
      if (octree->isNodeOccupied(*it)) {
        if (!publishMarkerArray) {
          continue;
        }

        // Ignore speckles in the map:
        if (do_filter_speckles_ &&
            (it.getDepth() == tree_depth_ + 1) &&
            isSpeckleNode(it.getKey())) {
          continue;
        }  // else: current octree node is no speckle, send it out

        double x = it.getX();
        double y = it.getY();
        double z = it.getZ();

        if (instance_id == -1) {
          bool is_occupied_by_fg = false;
          for (const auto& kv : octrees_) {
            if (kv.first == -1) {
              continue;
            }
            octomap::OcTreeNode* node = kv.second->search(x, y, z, /*depth=*/0);
            if ((node != NULL) && (node->getOccupancy() > 0.5)) {
              is_occupied_by_fg = true;
              break;
            }
          }
          if (is_occupied_by_fg) {
            continue;
          }
        }

        geometry_msgs::Point cubeCenter;
        cubeCenter.x = x;
        cubeCenter.y = y;
        cubeCenter.z = z;
        occupiedNodesVis.markers[it.getDepth()].points.push_back(cubeCenter);
      } else if ((instance_id == -1)) {
        if (!publishFreeMarkerArray) {
          continue;
        }

        geometry_msgs::Point cubeCenter;
        cubeCenter.x = it.getX();
        cubeCenter.y = it.getY();
        cubeCenter.z = it.getZ();
        freeNodesVis.markers[it.getDepth()].points.push_back(cubeCenter);
      }
    }

    // finish MarkerArray:
    if (publishMarkerArray) {
      for (unsigned i= 0; i < occupiedNodesVis.markers.size(); ++i) {
        double size = octree->getNodeSize(i);

        occupiedNodesVis.markers[i].header.frame_id = frame_id_world_;
        occupiedNodesVis.markers[i].header.stamp = rostime;
        occupiedNodesVis.markers[i].ns = boost::lexical_cast<std::string>(instance_id);
        occupiedNodesVis.markers[i].id = i;
        occupiedNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
        occupiedNodesVis.markers[i].scale.x = size;
        occupiedNodesVis.markers[i].scale.y = size;
        occupiedNodesVis.markers[i].scale.z = size;
        occupiedNodesVis.markers[i].color =
          morefusion_ros::utils::colorCategory40(instance_id + 1);
        occupiedNodesVis.markers[i].color.a = 0.5;
        occupiedNodesVis.markers[i].pose.orientation.w = 1;

        if (occupiedNodesVis.markers[i].points.size() > 0)
          occupiedNodesVis.markers[i].action = visualization_msgs::Marker::ADD;
        else
          occupiedNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;
      }
    }
    occupiedNodesVisAll.insert(std::make_pair(instance_id, occupiedNodesVis));
  }

  if (publishMarkerArray) {
    visualization_msgs::MarkerArray occupiedNodesVisBG;
    visualization_msgs::MarkerArray occupiedNodesVisFG;
    for (std::map<int, visualization_msgs::MarkerArray>::iterator it = occupiedNodesVisAll.begin();
         it != occupiedNodesVisAll.end(); it++) {
      const int instance_id = it->first;
      for (size_t j = 0; j < it->second.markers.size(); j++) {
        if (instance_id == -1) {
          occupiedNodesVisBG.markers.push_back(it->second.markers[j]);
        } else {
          occupiedNodesVisFG.markers.push_back(it->second.markers[j]);
        }
      }
    }
    pub_markers_bg_.publish(occupiedNodesVisBG);
    pub_markers_fg_.publish(occupiedNodesVisFG);
  }

  // finish FreeMarkerArray:
  if (publishFreeMarkerArray) {
    OcTreeT* octree_bg = octrees_.find(-1)->second;
    for (unsigned i= 0; i < freeNodesVis.markers.size(); ++i) {
      double size = octree_bg->getNodeSize(i);

      freeNodesVis.markers[i].header.frame_id = frame_id_world_;
      freeNodesVis.markers[i].header.stamp = rostime;
      freeNodesVis.markers[i].ns = "map";
      freeNodesVis.markers[i].id = i;
      freeNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
      freeNodesVis.markers[i].scale.x = size;
      freeNodesVis.markers[i].scale.y = size;
      freeNodesVis.markers[i].scale.z = size;
      freeNodesVis.markers[i].color.r = 0.5;
      freeNodesVis.markers[i].color.g = 0.5;
      freeNodesVis.markers[i].color.b = 0.5;
      freeNodesVis.markers[i].color.a = 1.0;


      if (freeNodesVis.markers[i].points.size() > 0) {
        freeNodesVis.markers[i].action = visualization_msgs::Marker::ADD;
      } else {
        freeNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;
      }
    }

    pub_markers_free_.publish(freeNodesVis);
  }

  if (publishBinaryMap) {
    publishBinaryOctoMap(rostime);
  }

  if (publishFullMap) {
    publishFullOctoMap(rostime);
  }
}


void OctomapServer::publishBinaryOctoMap(const ros::Time& rostime) const {
  Octomap map;
  map.header.frame_id = frame_id_world_;
  map.header.stamp = rostime;

  OcTreeT* octree_bg = octrees_.find(-1)->second;
  if (octomap_msgs::binaryMapToMsg(*octree_bg, map)) {
    pub_binary_map_.publish(map);
  } else {
    ROS_ERROR("Error serializing OctoMap");
  }
}

void OctomapServer::publishFullOctoMap(const ros::Time& rostime) const {
  Octomap map;
  map.header.frame_id = frame_id_world_;
  map.header.stamp = rostime;

  OcTreeT* octree_bg = octrees_.find(-1)->second;
  if (octomap_msgs::fullMapToMsg(*octree_bg, map)) {
    pub_full_map_.publish(map);
  } else {
    ROS_ERROR("Error serializing OctoMap");
  }
}

bool OctomapServer::isSpeckleNode(const octomap::OcTreeKey& nKey) const {
  octomap::OcTreeKey key;
  bool neighborFound = false;
  OcTreeT* octree_bg = octrees_.find(-1)->second;
  for (key[2] = nKey[2] - 1; !neighborFound && key[2] <= nKey[2] + 1; ++key[2]) {
    for (key[1] = nKey[1] - 1; !neighborFound && key[1] <= nKey[1] + 1; ++key[1]) {
      for (key[0] = nKey[0] - 1; !neighborFound && key[0] <= nKey[0] + 1; ++key[0]) {
        if (key != nKey) {
          octomap::OcTreeNode* node = octree_bg->search(key);
          if (node && octree_bg->isNodeOccupied(node)) {
            // we have a neighbor => break!
            neighborFound = true;
          }
        }
      }
    }
  }

  return neighborFound;
}

}  // namespace morefusion_ros

int main(int argc, char** argv) {
  ros::init(argc, argv, "octomap_server");
  morefusion_ros::OctomapServer server;
  ros::spin();
  return 0;
}
