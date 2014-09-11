
#include <ObjRecRANSAC/ObjRecRANSAC.h>
#include <ObjRecRANSAC/Shapes/PointSetShape.h>
#include <BasicTools/DataStructures/PointSet.h>
#include <BasicToolsL1/Vector.h>
#include <BasicToolsL1/Matrix.h>
#include <BasicTools/ComputationalGeometry/Algorithms/RANSACPlaneDetector.h>
#include <VtkBasics/VtkWindow.h>
#include <vtkPolyDataWriter.h>
#include <vtkPolyData.h>
#include <vtkCommand.h>
#include <vtkPoints.h>
#include <list>

#include <iostream>
#include <pcl/ModelCoefficients.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/extract_indices.h>


#include <resource_retriever/retriever.h>

#include <ros/ros.h>
#include <ros/exceptions.h>

#include <tf/tf.h>
#include <geometry_msgs/Pose.h>
#include <visualization_msgs/MarkerArray.h>
#include <sensor_msgs/PointCloud2.h>

#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>

#include <dynamic_reconfigure/server.h>

#include <objrec_msgs/PointSetShape.h>
#include <objrec_msgs/RecognizedObjects.h>
#include <objrec_msgs/ObjRecConfig.h>

#include <objrec_ros_integration/objrec_interface.h>

// Helper function for raising an exception if a required parameter is not found
template <class T>
static void require_param(const ros::NodeHandle &nh, const std::string &param_name, T &var)
{
  if(!nh.getParam(param_name, var)) {
    ROS_FATAL_STREAM("Required parameter not found! Namespace: "<<nh.getNamespace()<<" Parameter: "<<param_name);
    throw ros::InvalidParameterException("Parameter not found!");
  }
}

static void array_to_pose(const double* array, geometry_msgs::Pose &pose_msg)
{
  tf::Matrix3x3 rot_m =  tf::Matrix3x3(
      array[0],array[1],array[2],
      array[3],array[4],array[5],
      array[6],array[7],array[8]);
  tf::Quaternion rot_q;
  rot_m.getRotation(rot_q);
  tf::quaternionTFToMsg(rot_q, pose_msg.orientation);

  pose_msg.position.x = array[9] / 1000.0;
  pose_msg.position.y = array[10] / 1000.0;
  pose_msg.position.z = array[11] / 1000.0;
}

using namespace objrec_ros_integration;

ObjRecInterface::ObjRecInterface(ros::NodeHandle nh) :
  nh_(nh),
  listener_(nh, ros::Duration(20.0)),
  reconfigure_server_(nh),
  publish_markers_enabled_(false),
  n_clouds_per_recognition_(1),
  downsample_voxel_size_(3.5),
  confidence_time_multiplier_(30),
  scene_points_(vtkPoints::New(VTK_DOUBLE)),
  time_to_stop_(false)
{
  // Interface configuration
  nh.getParam("publish_markers", publish_markers_enabled_);
  nh.getParam("n_clouds_per_recognition", n_clouds_per_recognition_);
  nh.getParam("downsample_voxel_size", downsample_voxel_size_);

  nh.getParam("x_clip_min", x_clip_min_);
  nh.getParam("x_clip_max", x_clip_max_);
  nh.getParam("y_clip_min", y_clip_min_);
  nh.getParam("y_clip_max", y_clip_max_);
  nh.getParam("z_clip_min", z_clip_min_);
  nh.getParam("z_clip_max", z_clip_max_);

  // Get construction parameters from ROS & construct object recognizer
  require_param(nh,"pair_width",pair_width_);
  require_param(nh,"voxel_size",voxel_size_);
  
  objrec_.reset(new ObjRecRANSAC(pair_width_, voxel_size_, 0.5));

  // Get post-construction parameters from ROS
  require_param(nh,"object_visibility",object_visibility_);
  require_param(nh,"relative_object_size",relative_object_size_);
  require_param(nh,"relative_number_of_illegal_points",relative_number_of_illegal_points_);
  require_param(nh,"z_distance_threshold_as_voxel_size_fraction",z_distance_threshold_as_voxel_size_fraction_);
  require_param(nh,"normal_estimation_radius",normal_estimation_radius_);
  require_param(nh,"intersection_fraction",intersection_fraction_);
  require_param(nh,"num_threads",num_threads_);

	objrec_->setVisibility(object_visibility_);
	objrec_->setRelativeObjectSize(relative_object_size_);
	objrec_->setRelativeNumberOfIllegalPoints(relative_number_of_illegal_points_);
	objrec_->setZDistanceThreshAsVoxelSizeFraction(z_distance_threshold_as_voxel_size_fraction_); // 1.5*params.voxelSize
	objrec_->setNormalEstimationRadius(normal_estimation_radius_);
	objrec_->setIntersectionFraction(intersection_fraction_);
	objrec_->setNumberOfThreads(num_threads_);

  // Get model info from rosparam
  this->load_models_from_rosparam(); 

  // Get additional parameters from ROS
  require_param(nh,"success_probability",success_probability_);
  require_param(nh,"use_only_points_above_plane",use_only_points_above_plane_);

  // Plane detection parameters
  require_param(nh,"plane_thickness",plane_thickness_);
  require_param(nh,"rel_num_of_plane_points",rel_num_of_plane_points_);

  // Construct subscribers and publishers
  cloud_sub_ = nh.subscribe("points", 1, &ObjRecInterface::cloud_cb, this);
  pcl_cloud_sub_ = nh.subscribe("pcl_points", 1, &ObjRecInterface::pcl_cloud_cb, this);
  objects_pub_ = nh.advertise<objrec_msgs::RecognizedObjects>("recognized_objects",20);
  markers_pub_ = nh.advertise<visualization_msgs::MarkerArray>("recognized_objects_markers",20);
  foreground_points_pub_ = nh.advertise<pcl::PointCloud<pcl::PointXYZ> >("foreground_points",10);

  // Set up dynamic reconfigure
  reconfigure_server_.setCallback(boost::bind(&ObjRecInterface::reconfigure_cb, this, _1, _2));

  // Start recognition thread
  recognition_thread_.reset(new boost::thread(boost::bind(&ObjRecInterface::recognize_objects, this)));

  ROS_INFO_STREAM("Constructed ObjRec interface.");
}

ObjRecInterface::~ObjRecInterface() { 
  time_to_stop_ = true;
  recognition_thread_->join();
}

void ObjRecInterface::load_models_from_rosparam()
{
  ROS_INFO_STREAM("Loading models from rosparam...");

  // Get the list of model param names
  XmlRpc::XmlRpcValue objrec_models_xml;
  nh_.param("models", objrec_models_xml, objrec_models_xml);

  // Iterate through the models 
  for(int i =0; i < objrec_models_xml.size(); i++) {
    std::string model_label = static_cast<std::string>(objrec_models_xml[i]);

    // Get the mesh uri & store it
    require_param(nh_,"model_uris/"+model_label,model_uris_[model_label]);
    // TODO: make this optional
    require_param(nh_,"stl_uris/"+model_label,stl_uris_[model_label]);

    // Add the model
    this->add_model(model_label, model_uris_[model_label]);
  }
}

void ObjRecInterface::add_model(
    const std::string &model_label,
    const std::string &model_uri)
{
  ROS_INFO_STREAM("Adding model \""<<model_label<<"\" from "<<model_uri);
  // Fetch the model data with a ros resource retriever
  resource_retriever::Retriever retriever;
  resource_retriever::MemoryResource resource;

  try {
    resource = retriever.get(model_uri); 
  } catch (resource_retriever::Exception& e) {
    ROS_ERROR_STREAM("Failed to retrieve \""<<model_label<<"\" model file from \""<<model_uri<<"\" error: "<<e.what());
    return;
  }

  // Load the model into objrec
  vtkSmartPointer<vtkPolyDataReader> reader =
    vtkSmartPointer<vtkPolyDataReader>::New();
  // This copies the data from the resource structure into the polydata reader
  reader->SetBinaryInputString(
      (const char*)resource.data.get(),
      resource.size);
  reader->ReadFromInputStringOn();
  reader->Update();
  readers_.push_back(reader);
  
  // Create new model user data
  boost::shared_ptr<UserData> user_data(new UserData());
  user_data->setLabel(model_label.c_str());
  user_data_list_.push_back(user_data);

  // Add the model to the model library
  objrec_->addModel(reader->GetOutput(), user_data.get());
}

void ObjRecInterface::reconfigure_cb(objrec_msgs::ObjRecConfig &config, uint32_t level)
{
  ROS_DEBUG("Reconfigure Request!");
  object_visibility_ = config.object_visibility;
  relative_object_size_ = config.relative_object_size;
  relative_number_of_illegal_points_ = config.relative_number_of_illegal_points;
  z_distance_threshold_as_voxel_size_fraction_ = config.z_distance_threshold_as_voxel_size_fraction; // 1.5*params.voxelSize
  normal_estimation_radius_ = config.normal_estimation_radius;
  intersection_fraction_ = config.intersection_fraction;
  num_threads_ = config.num_threads;

	objrec_->setVisibility(object_visibility_);
	objrec_->setRelativeObjectSize(relative_object_size_);
	objrec_->setRelativeNumberOfIllegalPoints(relative_number_of_illegal_points_);
	objrec_->setZDistanceThreshAsVoxelSizeFraction(z_distance_threshold_as_voxel_size_fraction_); // 1.5*params.voxelSize
	objrec_->setNormalEstimationRadius(normal_estimation_radius_);
	objrec_->setIntersectionFraction(intersection_fraction_);
	objrec_->setNumberOfThreads(num_threads_);

  // Other parameters
  use_only_points_above_plane_ = config.use_only_points_above_plane;
  n_clouds_per_recognition_ = config.n_clouds_per_recognition;
  publish_markers_enabled_ = config.publish_markers;
  downsample_voxel_size_ = config.downsample_voxel_size;
  confidence_time_multiplier_ = config.confidence_time_multiplier;

  x_clip_min_ = config.x_clip_min;
  x_clip_max_ = config.x_clip_max;
  y_clip_min_ = config.y_clip_min;
  y_clip_max_ = config.y_clip_max;
  z_clip_min_ = config.z_clip_min;
  z_clip_max_ = config.z_clip_max;
}

void ObjRecInterface::cloud_cb(const sensor_msgs::PointCloud2ConstPtr &points_msg)
{
  // Convert to PCL cloud
  boost::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB> > cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  pcl::fromROSMsg(*points_msg, *cloud);

  this->pcl_cloud_cb(cloud);
}

void ObjRecInterface::pcl_cloud_cb(const boost::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB> > &cloud)
{
  // Lock the buffer mutex while we're capturing a new point cloud
  boost::mutex::scoped_lock buffer_lock(buffer_mutex_);

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_clipped(new pcl::PointCloud<pcl::PointXYZRGB>());
  cloud_clipped->header = cloud->header;
  for (int j = 0; j < (int) cloud->points.size(); ++j) {
    if (cloud->points[j].x > x_clip_min_ && cloud->points[j].x < x_clip_max_ &&
        cloud->points[j].y > y_clip_min_ && cloud->points[j].y < y_clip_max_ &&
        cloud->points[j].z > z_clip_min_ && cloud->points[j].z < z_clip_max_) 
    {
      // Add point
      cloud_clipped->push_back(cloud->points[j]);
    } 
  }

  // Store the cloud
  clouds_.push(cloud_clipped);

  // Increment the cloud index
  if(clouds_.size() > (unsigned)n_clouds_per_recognition_) {
    clouds_.pop();
  }

  //foreground_points_pub_.publish(cloud_clipped);
}

void ObjRecInterface::recognize_objects() 
{
  ros::Rate max_rate(100.0);

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_full(new pcl::PointCloud<pcl::PointXYZRGB>());
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZRGB>());
  boost::shared_ptr<pcl::VoxelGrid<pcl::PointXYZRGB> > voxel_grid(new pcl::VoxelGrid<pcl::PointXYZRGB>());

  pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
  pcl::PointIndices::Ptr outliers (new pcl::PointIndices);
  
  // Create point clouds for foreground and background points
  vtkSmartPointer<vtkPoints> foreground_points(vtkPoints::New(VTK_DOUBLE));

  std::list<PointSetShape*> detected_models;

  while(ros::ok() && !time_to_stop_) 
  {
    // Don't hog the cpu
    max_rate.sleep();

    cloud_full->clear();
    cloud->clear();

    // Scope for syncrhonization
    ROS_DEBUG_STREAM("ObjRec: Aggregating point clouds... ");
    {
      // Lock the buffer mutex
      boost::mutex::scoped_lock buffer_lock(buffer_mutex_);

      // Continue if the cloud is empty
      static ros::Rate warn_rate(1.0);
      if(clouds_.empty()) {
        ROS_WARN("Pointcloud buffer is empty!");
        warn_rate.sleep();
        continue;
      }

      ROS_DEBUG_STREAM("ObjRec: Computing objects from "
          <<clouds_.size()<<" point clounds "
          <<"between "<<(ros::Time::now() - pcl_conversions::fromPCL(clouds_.back()->header).stamp)
          <<" to "<<(ros::Time::now() - pcl_conversions::fromPCL(clouds_.front()->header).stamp)<<" seconds after they were acquired.");

      // Copy references to the stored clouds
      cloud_full->header = clouds_.front()->header;

      while(!clouds_.empty()) {
        *cloud_full += *(clouds_.front());
        clouds_.pop();
      }
    }

    ROS_DEBUG_STREAM("ObjRec: Downsampling full cloud from "<<cloud_full->points.size()<<" points...");

    voxel_grid->setLeafSize(
        downsample_voxel_size_/1000.0,
        downsample_voxel_size_/1000.0,
        downsample_voxel_size_/1000.0);
    voxel_grid->setInputCloud(cloud_full);
    voxel_grid->filter(*cloud);

    ROS_DEBUG_STREAM("ObjRec: Downsampled cloud has "<<cloud->points.size()<<" points.");



    ROS_DEBUG("ObjRec: Removing points not above plane with PCL...");

    // Create the segmentation object
    pcl::SACSegmentation<pcl::PointXYZRGB> seg;
    // Optional
    seg.setOptimizeCoefficients (true);
    // Mandatory
    seg.setModelType (pcl::SACMODEL_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setDistanceThreshold (0.01);

    seg.setInputCloud (cloud);
    seg.segment (*inliers, *coefficients);

    if (inliers->indices.size () == 0)
    {
      ROS_ERROR_STREAM("Could not estimate a planar model for the given dataset.");
      continue;
    }

    // Flip plane if it's pointing away
    if(coefficients->values[2] > 0.0) {
      coefficients->values[0] *= -1.0;
      coefficients->values[1] *= -1.0;
      coefficients->values[2] *= -1.0;
      coefficients->values[3] *= -1.0;
    }

    // Remove the planar inliers, extract the rest
    pcl::ExtractIndices<pcl::PointXYZRGB> extract;
    extract.setInputCloud(cloud);
    extract.setIndices(inliers);
    extract.setNegative(true);
    extract.filter(*cloud);

    foreground_points->SetNumberOfPoints(cloud->points.size());
    foreground_points->Reset();

    // Require the points are inside of the clopping box
    for (pcl::PointCloud<pcl::PointXYZRGB>::const_iterator it = cloud->begin();
         it != cloud->end();
         ++it)
    {
      const double dist = (
          it->x * coefficients->values[0] +
          it->y * coefficients->values[1] +
          it->z * coefficients->values[2]) + coefficients->values[3];

      if(dist > plane_thickness_/1000.0/2.0) {
        // Add point if it's above the plane
        foreground_points->InsertNextPoint(
            it->x * 1000.0,
            it->y * 1000.0,
            it->z * 1000.0);
      }
    }

    // Detect models
    ROS_DEBUG_STREAM("ObjRec: Attempting recognition...");
    detected_models.clear();
    objrec_->doRecognition(foreground_points, success_probability_, detected_models);

    ROS_DEBUG("ObjRec: Seconds elapsed = %.2lf", objrec_->getLastOverallRecognitionTimeSec());
    ROS_DEBUG("ObjRec: Seconds per hypothesis = %.6lf", objrec_->getLastOverallRecognitionTimeSec()
        / (double) objrec_->getLastNumberOfCheckedHypotheses());

    // Construct recognized objects message
    objrec_msgs::RecognizedObjects objects_msg;
    objects_msg.header.stamp = pcl_conversions::fromPCL(cloud->header).stamp;
    objects_msg.header.frame_id = "/world";//cloud->header;

    for(std::list<PointSetShape*>::iterator it = detected_models.begin();
        it != detected_models.end();
        ++it)
    {
      PointSetShape *detected_model = *it;

      // Construct and populate a message
      objrec_msgs::PointSetShape pss_msg;
      pss_msg.label = detected_model->getUserData()->getLabel();
      pss_msg.confidence = detected_model->getConfidence();
      array_to_pose(detected_model->getRigidTransform(), pss_msg.pose);

      // Transform into the world frame TODO: make this frame a parameter
      geometry_msgs::PoseStamped pose_stamped_in, pose_stamped_out;
      pose_stamped_in.header = pcl_conversions::fromPCL(cloud->header);
      pose_stamped_in.pose = pss_msg.pose;

      try {
        listener_.transformPose("/world",pose_stamped_in,pose_stamped_out);
        pss_msg.pose = pose_stamped_out.pose;
      }
      catch (tf::TransformException ex){
        ROS_WARN("Not transforming recognized objects into world frame: %s",ex.what());
      }

      objects_msg.objects.push_back(pss_msg);
      delete *it;
    }

    // Publish the visualization markers
    this->publish_markers(objects_msg);

    // Publish the recognized objects
    objects_pub_.publish(objects_msg);

    // Publish the points used in the scan, for debugging
    foreground_points_pub_.publish(cloud);
  }
}

void ObjRecInterface::publish_markers(const objrec_msgs::RecognizedObjects &objects_msg)
{
  visualization_msgs::MarkerArray marker_array;
  int id = 0;

  for(std::vector<objrec_msgs::PointSetShape>::const_iterator it = objects_msg.objects.begin();
      it != objects_msg.objects.end();
      ++it)
  {
    visualization_msgs::Marker marker;

    marker.header = objects_msg.header;
    marker.type = visualization_msgs::Marker::MESH_RESOURCE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.lifetime = ros::Duration(confidence_time_multiplier_*it->confidence);
    marker.ns = "objrec";
    marker.id = 0;

    marker.scale.x = 0.001;
    marker.scale.y = 0.001;
    marker.scale.z = 0.001;

    marker.color.a = 0.75;
    marker.color.r = 1.0;
    marker.color.g = 0.1;
    marker.color.b = 0.3;

    marker.id = id++;
    marker.pose = it->pose;
    marker.mesh_resource = stl_uris_[it->label];

    marker_array.markers.push_back(marker);
  }

  // Publish the markers
  markers_pub_.publish(marker_array);
}

