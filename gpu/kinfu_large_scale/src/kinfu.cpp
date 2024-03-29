/*
 * Software License Agreement (BSD License)
 *
 *  Point Cloud Library (PCL) - www.pointclouds.org
 *  Copyright (c) 2011, Willow Garage, Inc.
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <iostream>
#include <algorithm>

#include <pcl/common/time.h>
#include <pcl/gpu/kinfu_large_scale/kinfu.h>
#include "internal.h"


#include <Eigen/Core>
#include <Eigen/SVD>
#include <Eigen/Cholesky>
#include <Eigen/Geometry>
#include <Eigen/LU>

#ifdef HAVE_OPENCV
  #include <opencv2/opencv.hpp>
  //~ #include <opencv2/gpu/gpu.hpp>
  //~ #include <pcl/gpu/utils/timers_opencv.hpp>
#endif

using namespace std;
using namespace pcl::device;
using namespace pcl::gpu;

using Eigen::AngleAxisf;
using Eigen::Array3f;
using Eigen::Vector3i;
using Eigen::Vector3f;

namespace pcl
{
  namespace gpu
  {
    Eigen::Vector3f rodrigues2(const Eigen::Matrix3f& matrix);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
pcl::gpu::KinfuTracker::KinfuTracker (const Eigen::Vector3f &volume_size, const float shiftingDistance, bool useVisualOdometry, int rows, int cols) : rows_(rows), cols_(cols), global_time_(0), max_icp_distance_(0), integration_metric_threshold_(0.f), cyclical_( DISTANCE_THRESHOLD, pcl::device::VOLUME_SIZE, VOLUME_X), perform_last_scan_ (false), finished_(false)
{
  //const Vector3f volume_size = Vector3f::Constant (VOLUME_SIZE);
  const Vector3i volume_resolution (VOLUME_X, VOLUME_Y, VOLUME_Z);

  volume_size_ = volume_size(0);

  tsdf_volume_ = TsdfVolume::Ptr ( new TsdfVolume(volume_resolution) );
  tsdf_volume_->setSize (volume_size);
  
  shifting_distance_ = shiftingDistance;

  // set cyclical buffer values
  cyclical_.setDistanceThreshold (shifting_distance_);
  cyclical_.setVolumeSize (volume_size_, volume_size_, volume_size_);
  
  setDepthIntrinsics (FOCAL_LENGTH, FOCAL_LENGTH); // default values, can be overwritten
  
  init_Rcam_ = Eigen::Matrix3f::Identity ();// * AngleAxisf(-30.f/180*3.1415926, Vector3f::UnitX());
  init_tcam_ = volume_size * 0.5f - Vector3f (0, 0, volume_size (2) / 2 * 1.2f);

  const int iters[] = {10, 5, 4};
  std::copy (iters, iters + LEVELS, icp_iterations_);

  const float default_distThres = 0.10f; //meters
  const float default_angleThres = sin (20.f * 3.14159254f / 180.f);
  const float default_tranc_dist = 0.03f; //meters

  setIcpCorespFilteringParams (default_distThres, default_angleThres);
  tsdf_volume_->setTsdfTruncDist (default_tranc_dist);

  allocateBufffers (rows, cols);

  rmats_.reserve (30000);
  tvecs_.reserve (30000);
  
  reset ();
  
  // initialize cyclical buffer
  cyclical_.initBuffer(tsdf_volume_);
  use_visual_odometry_ = useVisualOdometry;//delete this one later
  
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::setDepthIntrinsics (float fx, float fy, float cx, float cy)
{
  fx_ = fx;
  fy_ = fy;
  cx_ = (cx == -1) ? cols_/2-0.5f : cx;
  cy_ = (cy == -1) ? rows_/2-0.5f : cy;  
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::setInitialCameraPose (const Eigen::Affine3f& pose)
{
  init_Rcam_ = pose.rotation ();
  init_tcam_ = pose.translation ();
  reset ();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::setDepthTruncationForICP (float max_icp_distance)
{
  max_icp_distance_ = max_icp_distance;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::setCameraMovementThreshold(float threshold)
{
  integration_metric_threshold_ = threshold;  
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::setIcpCorespFilteringParams (float distThreshold, float sineOfAngle)
{
  distThres_  = distThreshold; //mm
  angleThres_ = sineOfAngle;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int
pcl::gpu::KinfuTracker::cols ()
{
  return (cols_);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int
pcl::gpu::KinfuTracker::rows ()
{
  return (rows_);
}

void
pcl::gpu::KinfuTracker::extractAndMeshWorld ()
{
  finished_ = true;
  int cloud_size = 0;
  cloud_size = cyclical_.getWorldModel ()->getWorld ()->points.size();
  
  if (cloud_size <= 0)
  {
	PCL_WARN ("World model currently has no points. Skipping save procedure.\n");
	return;
  }
  else
  {
	PCL_INFO ("Saving current world to world.pcd with %d points.\n", cloud_size);
	pcl::io::savePCDFile<pcl::PointXYZI> ("world.pcd", *(cyclical_.getWorldModel ()->getWorld ()), true);
	return;
  }
  
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::reset ()
{
  if (global_time_)
    PCL_WARN ("Reset\n");
    
  // dump current world to a pcd file
  /*
  if (global_time_)
  {
    PCL_INFO ("Saving current world to current_world.pcd\n");
    pcl::io::savePCDFile<pcl::PointXYZI> ("current_world.pcd", *(cyclical_.getWorldModel ()->getWorld ()), true);
    // clear world model
    cyclical_.getWorldModel ()->reset ();
  }
  */
   
  global_time_ = 0;
  rmats_.clear ();
  tvecs_.clear ();

  rmats_.push_back (init_Rcam_);
  tvecs_.push_back (init_tcam_);

  tsdf_volume_->reset ();
  
  // reset cyclical buffer as well
  cyclical_.resetBuffer (tsdf_volume_);
  

   

    
  if (color_volume_) // color integration mode is enabled
    color_volume_->reset ();    
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::allocateBufffers (int rows, int cols)
{    
  depths_curr_.resize (LEVELS);
  vmaps_g_curr_.resize (LEVELS);
  nmaps_g_curr_.resize (LEVELS);

  vmaps_g_prev_.resize (LEVELS);
  nmaps_g_prev_.resize (LEVELS);

  vmaps_curr_.resize (LEVELS);
  nmaps_curr_.resize (LEVELS);

  coresps_.resize (LEVELS);

  for (int i = 0; i < LEVELS; ++i)
  {
    int pyr_rows = rows >> i;
    int pyr_cols = cols >> i;

    depths_curr_[i].create (pyr_rows, pyr_cols);

    vmaps_g_curr_[i].create (pyr_rows*3, pyr_cols);
    nmaps_g_curr_[i].create (pyr_rows*3, pyr_cols);

    vmaps_g_prev_[i].create (pyr_rows*3, pyr_cols);
    nmaps_g_prev_[i].create (pyr_rows*3, pyr_cols);

    vmaps_curr_[i].create (pyr_rows*3, pyr_cols);
    nmaps_curr_[i].create (pyr_rows*3, pyr_cols);

    coresps_[i].create (pyr_rows, pyr_cols);
  }  
  depthRawScaled_.create (rows, cols);
  // see estimate tranform for the magic numbers
  gbuf_.create (27, 20*60);
  sumbuf_.create (27);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool
pcl::gpu::KinfuTracker::operator() (const DepthMap& depth_raw)
{  
  std::cout << "Original algo 2" << std::endl;
  device::Intr intr (fx_, fy_, cx_, cy_);
  {
    //ScopeTime time(">>> Bilateral, pyr-down-all, create-maps-all");
    //depth_raw.copyTo(depths_curr[0]);
    device::bilateralFilter (depth_raw, depths_curr_[0]);

	if (max_icp_distance_ > 0)
		device::truncateDepth(depths_curr_[0], max_icp_distance_);

    for (int i = 1; i < LEVELS; ++i)
      device::pyrDown (depths_curr_[i-1], depths_curr_[i]);

    for (int i = 0; i < LEVELS; ++i)
    {
      device::createVMap (intr(i), depths_curr_[i], vmaps_curr_[i]);
      //device::createNMap(vmaps_curr_[i], nmaps_curr_[i]);
      computeNormalsEigen (vmaps_curr_[i], nmaps_curr_[i]);
    }
    pcl::device::sync ();
  }

  //can't perform more on first frame
  if (global_time_ == 0)
  {
    
    Matrix3frm initial_cam_rot = rmats_[0]; //  [Ri|ti] - pos of camera, i.e.
    Matrix3frm initial_cam_rot_inv = initial_cam_rot.inverse ();
    Vector3f   initial_cam_trans = tvecs_[0]; //  transform from camera to global coo space for (i-1)th camera pose
        
    Mat33&  device_initial_cam_rot = device_cast<Mat33> (initial_cam_rot);
    Mat33&  device_initial_cam_rot_inv = device_cast<Mat33> (initial_cam_rot_inv);
    float3& device_initial_cam_trans = device_cast<float3>(initial_cam_trans);
         
    float3 device_volume_size = device_cast<const float3>(tsdf_volume_->getSize());

    device::integrateTsdfVolume(depth_raw, intr, device_volume_size, device_initial_cam_rot_inv, device_initial_cam_trans, tsdf_volume_->getTsdfTruncDist(), tsdf_volume_->data(), getCyclicalBufferStructure (), depthRawScaled_);
    
    /*
    Matrix3frm init_Rcam = rmats_[0]; //  [Ri|ti] - pos of camera, i.e.
    Vector3f   init_tcam = tvecs_[0]; //  transform from camera to global coo space for (i-1)th camera pose

    Mat33&  device_Rcam = device_cast<Mat33> (init_Rcam);
    float3& device_tcam = device_cast<float3>(init_tcam);

    Matrix3frm init_Rcam_inv = init_Rcam.inverse ();
    Mat33&   device_Rcam_inv = device_cast<Mat33> (init_Rcam_inv);
    float3 device_volume_size = device_cast<const float3>(tsdf_volume_->getSize ());

    //integrateTsdfVolume(depth_raw, intr, device_volume_size, device_Rcam_inv, device_tcam, tranc_dist, volume_);    
    device::integrateTsdfVolume(depth_raw, intr, device_volume_size, device_Rcam_inv, device_tcam, tsdf_volume_->getTsdfTruncDist (), tsdf_volume_->data (), getCyclicalBufferStructure (), depthRawScaled_);
    */
    
    for (int i = 0; i < LEVELS; ++i)
      device::tranformMaps (vmaps_curr_[i], nmaps_curr_[i], device_initial_cam_rot, device_initial_cam_trans, vmaps_g_prev_[i], nmaps_g_prev_[i]);


    if(perform_last_scan_)
      finished_ = true;
      

    ++global_time_;
    return (false);
  }

  ///////////////////////////////////////////////////////////////////////////////////////////
  // Iterative Closest Point
  
  
  // GET PREVIOUS GLOBAL TRANSFORM
  // Previous global rotation
  Matrix3frm cam_rot_global_prev = rmats_[global_time_ - 1];            // [Ri|ti] - pos of camera, i.e.
  // Previous global translation
  Vector3f   cam_trans_global_prev = tvecs_[global_time_ - 1];          // transform from camera to global coo space for (i-1)th camera pose
  // Previous global inverse rotation
  Matrix3frm cam_rot_global_prev_inv = cam_rot_global_prev.inverse ();  // Rprev.t();
  
  // GET CURRENT GLOBAL TRANSFORM
  Matrix3frm cam_rot_global_curr = cam_rot_global_prev;                 // transform to global coo for ith camera pose
  Vector3f   cam_trans_global_curr = cam_trans_global_prev;
  
  // CONVERT TO DEVICE TYPES 
  //LOCAL PREVIOUS TRANSFORM
  Mat33&  device_cam_rot_local_prev_inv = device_cast<Mat33> (cam_rot_global_prev_inv);

  float3& device_cam_trans_local_prev_tmp = device_cast<float3> (cam_trans_global_prev);
  float3 device_cam_trans_local_prev;
  device_cam_trans_local_prev.x = device_cam_trans_local_prev_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
  device_cam_trans_local_prev.y = device_cam_trans_local_prev_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
  device_cam_trans_local_prev.z = device_cam_trans_local_prev_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;
 
  {
    //ScopeTime time("icp-all");
    for (int level_index = LEVELS-1; level_index>=0; --level_index)
    {
      int iter_num = icp_iterations_[level_index];
      
      // current maps
      MapArr& vmap_curr = vmaps_curr_[level_index];
      MapArr& nmap_curr = nmaps_curr_[level_index];   
      
      // previous maps
      MapArr& vmap_g_prev = vmaps_g_prev_[level_index];
      MapArr& nmap_g_prev = nmaps_g_prev_[level_index];
      
      // We need to transform the maps from global to the local coordinates
      Mat33&  rotation_id = device_cast<Mat33> (rmats_[0]); // Identity Rotation Matrix. Because we only need translation
      float3 cube_origin = (getCyclicalBufferStructure ())->origin_metric;
      cube_origin.x = -cube_origin.x;
      cube_origin.y = -cube_origin.y;
      cube_origin.z = -cube_origin.z;
      
      MapArr& vmap_temp = vmap_g_prev;
      MapArr& nmap_temp = nmap_g_prev;
      device::tranformMaps (vmap_temp, nmap_temp, rotation_id, cube_origin, vmap_g_prev, nmap_g_prev); 

      for (int iter = 0; iter < iter_num; ++iter)
      {

        //CONVERT TO DEVICE TYPES
        // CURRENT LOCAL TRANSFORM
        Mat33&  device_cam_rot_local_curr = device_cast<Mat33> (cam_rot_global_curr);/// We have not dealt with changes in rotations
                          
        float3& device_cam_trans_local_curr_tmp = device_cast<float3> (cam_trans_global_curr);
        float3 device_cam_trans_local_curr; 
        device_cam_trans_local_curr.x = device_cam_trans_local_curr_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
        device_cam_trans_local_curr.y = device_cam_trans_local_curr_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
        device_cam_trans_local_curr.z = device_cam_trans_local_curr_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;
        
        Eigen::Matrix<double, 6, 6, Eigen::RowMajor> A;
        Eigen::Matrix<double, 6, 1> b;

        estimateCombined (device_cam_rot_local_curr, device_cam_trans_local_curr, vmap_curr, nmap_curr, device_cam_rot_local_prev_inv, device_cam_trans_local_prev, intr (level_index), 
                          vmap_g_prev, nmap_g_prev, distThres_, angleThres_, gbuf_, sumbuf_, A.data (), b.data ());

        //checking nullspace
        double det = A.determinant ();
  
		if ( fabs (det) < 1e-15 || pcl_isnan (det) )
        {
          if (pcl_isnan (det)) cout << "qnan" << endl;
          
          PCL_ERROR ("LOST...\n");
          reset ();
          return (false);
        }

        Eigen::Matrix<float, 6, 1> result = A.llt ().solve (b).cast<float>();

        float alpha = result (0);
        float beta  = result (1);
        float gamma = result (2);

        Eigen::Matrix3f cam_rot_incremental = (Eigen::Matrix3f)AngleAxisf (gamma, Vector3f::UnitZ ()) * AngleAxisf (beta, Vector3f::UnitY ()) * AngleAxisf (alpha, Vector3f::UnitX ());
        Vector3f cam_trans_incremental = result.tail<3> ();

        //compose
        cam_trans_global_curr = cam_rot_incremental * cam_trans_global_curr + cam_trans_incremental;
        cam_rot_global_curr = cam_rot_incremental * cam_rot_global_curr;

      }
    }
  }
  //save tranform
  rmats_.push_back (cam_rot_global_curr); 
  tvecs_.push_back (cam_trans_global_curr);
  
  //check for shift
  bool has_shifted = cyclical_.checkForShift(tsdf_volume_, getCameraPose (), 0.6 * volume_size_, true, perform_last_scan_);

  if(has_shifted)
    PCL_WARN ("SHIFTING\n");
    
  // get NEW local rotation 
  Matrix3frm cam_rot_local_curr_inv = cam_rot_global_curr.inverse ();
  Mat33&  device_cam_rot_local_curr_inv = device_cast<Mat33> (cam_rot_local_curr_inv);
  Mat33&  device_cam_rot_local_curr = device_cast<Mat33> (cam_rot_global_curr); 
  
  // get NEW local translation
  float3& device_cam_trans_local_curr_tmp = device_cast<float3> (cam_trans_global_curr);
  float3 device_cam_trans_local_curr;
  device_cam_trans_local_curr.x = device_cam_trans_local_curr_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
  device_cam_trans_local_curr.y = device_cam_trans_local_curr_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
  device_cam_trans_local_curr.z = device_cam_trans_local_curr_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;  
  
  
  ///////////////////////////////////////////////////////////////////////////////////////////
  // Integration check - We do not integrate volume if camera does not move.  
  float rnorm = rodrigues2(cam_rot_global_curr.inverse() * cam_rot_global_prev).norm();
  float tnorm = (cam_trans_global_curr - cam_trans_global_prev).norm();    
  const float alpha = 1.f;
  bool integrate = (rnorm + alpha * tnorm)/2 >= integration_metric_threshold_;
  //~ if(integrate)
    //~ std::cout << "\tCamera movement since previous frame was " << (rnorm + alpha * tnorm)/2 << " integrate is set to " << integrate << std::endl;
  //~ else
    //~ std::cout << "Camera movement since previous frame was " << (rnorm + alpha * tnorm)/2 << " integrate is set to " << integrate << std::endl;

  ///////////////////////////////////////////////////////////////////////////////////////////
  // Volume integration
  float3 device_volume_size = device_cast<const float3> (tsdf_volume_->getSize());
/*
  Matrix3frm Rcurr_inv = Rcurr.inverse ();
  Mat33&  device_Rcurr_inv = device_cast<Mat33> (Rcurr_inv);
  float3& device_tcurr = device_cast<float3> (tcurr);*/
  if (integrate)
  {
    //integrateTsdfVolume(depth_raw, intr, device_volume_size, device_Rcurr_inv, device_tcurr, tranc_dist, volume_);
    integrateTsdfVolume (depth_raw, intr, device_volume_size, device_cam_rot_local_curr_inv, device_cam_trans_local_curr, tsdf_volume_->getTsdfTruncDist (), tsdf_volume_->data (), getCyclicalBufferStructure (), depthRawScaled_);
  }

  ///////////////////////////////////////////////////////////////////////////////////////////
  // Ray casting
  /*Mat33& device_Rcurr = device_cast<Mat33> (Rcurr);*/
  {          
    raycast (intr, device_cam_rot_local_curr, device_cam_trans_local_curr, tsdf_volume_->getTsdfTruncDist (), device_volume_size, tsdf_volume_->data (), getCyclicalBufferStructure (), vmaps_g_prev_[0], nmaps_g_prev_[0]);
    
    // POST-PROCESSING: We need to transform the newly raycasted maps into the global space.
    Mat33&  rotation_id = device_cast<Mat33> (rmats_[0]); /// Identity Rotation Matrix. Because we only need translation
    float3 cube_origin = (getCyclicalBufferStructure ())->origin_metric;
    
    //~ PCL_INFO ("Raycasting with cube origin at %f, %f, %f\n", cube_origin.x, cube_origin.y, cube_origin.z);
    
    MapArr& vmap_temp = vmaps_g_prev_[0];
    MapArr& nmap_temp = nmaps_g_prev_[0];
    
    device::tranformMaps (vmap_temp, nmap_temp, rotation_id, cube_origin, vmaps_g_prev_[0], nmaps_g_prev_[0]);
    
    for (int i = 1; i < LEVELS; ++i)
    {
      resizeVMap (vmaps_g_prev_[i-1], vmaps_g_prev_[i]);
      resizeNMap (nmaps_g_prev_[i-1], nmaps_g_prev_[i]);
    }
    pcl::device::sync ();
  }

  if(has_shifted && perform_last_scan_)
    extractAndMeshWorld ();

  ++global_time_;
  return (true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Eigen::Affine3f
pcl::gpu::KinfuTracker::getCameraPose (int time) const
{
  if (time > (int)rmats_.size () || time < 0)
    time = rmats_.size () - 1;

  Eigen::Affine3f aff;
  aff.linear () = rmats_[time];
  aff.translation () = tvecs_[time];
  return (aff);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

size_t
pcl::gpu::KinfuTracker::getNumberOfPoses () const
{
  return rmats_.size();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const TsdfVolume& 
pcl::gpu::KinfuTracker::volume() const 
{ 
  return *tsdf_volume_; 
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TsdfVolume& 
pcl::gpu::KinfuTracker::volume()
{
  return *tsdf_volume_;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const ColorVolume& 
pcl::gpu::KinfuTracker::colorVolume() const
{
  return *color_volume_;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

ColorVolume& 
pcl::gpu::KinfuTracker::colorVolume()
{
  return *color_volume_;
}
     
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::getImage (View& view) const
{
  //Eigen::Vector3f light_source_pose = tsdf_volume_->getSize() * (-3.f);
  Eigen::Vector3f light_source_pose = tvecs_[tvecs_.size () - 1];

  device::LightSource light;
  light.number = 1;
  light.pos[0] = device_cast<const float3>(light_source_pose);

  view.create (rows_, cols_);
  generateImage (vmaps_g_prev_[0], nmaps_g_prev_[0], light, view);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::getLastFrameCloud (DeviceArray2D<PointType>& cloud) const
{
  cloud.create (rows_, cols_);
  DeviceArray2D<float4>& c = (DeviceArray2D<float4>&)cloud;
  device::convert (vmaps_g_prev_[0], c);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void
pcl::gpu::KinfuTracker::getLastFrameNormals (DeviceArray2D<NormalType>& normals) const
{
  normals.create (rows_, cols_);
  DeviceArray2D<float8>& n = (DeviceArray2D<float8>&)normals;
  device::convert (nmaps_g_prev_[0], n);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void
pcl::gpu::KinfuTracker::initColorIntegration(int max_weight)
{     
  color_volume_ = pcl::gpu::ColorVolume::Ptr( new ColorVolume(*tsdf_volume_, max_weight) );  
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool 
pcl::gpu::KinfuTracker::original_algo(const DepthMap& depth, const View& colors)
{ 
std::cout << "ORIGINAL ALGO" << std::endl;
  bool res = (*this)(depth);

  if (res && color_volume_)
  {
    const float3 device_volume_size = device_cast<const float3> (tsdf_volume_->getSize());
    device::Intr intr(fx_, fy_, cx_, cy_);

    Matrix3frm R_inv = rmats_.back().inverse();
    Vector3f   t     = tvecs_.back();
    
    Mat33&  device_Rcurr_inv = device_cast<Mat33> (R_inv);
    float3& device_tcurr = device_cast<float3> (t);
    
    device::updateColorVolume(intr, tsdf_volume_->getTsdfTruncDist(), device_Rcurr_inv, device_tcurr, vmaps_g_prev_[0], 
        colors, device_volume_size, color_volume_->data(), color_volume_->getMaxWeight());
  }

  return res;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace pcl
{
  namespace gpu
  {
    PCL_EXPORTS void 
    paint3DView(const KinfuTracker::View& rgb24, KinfuTracker::View& view, float colors_weight = 0.5f)
    {
      device::paint3DView(rgb24, view, colors_weight);
    }

    PCL_EXPORTS void
    mergePointNormal(const DeviceArray<PointXYZ>& cloud, const DeviceArray<Normal>& normals, DeviceArray<PointNormal>& output)
    {
      const size_t size = min(cloud.size(), normals.size());
      output.create(size);

      const DeviceArray<float4>& c = (const DeviceArray<float4>&)cloud;
      const DeviceArray<float8>& n = (const DeviceArray<float8>&)normals;
      const DeviceArray<float12>& o = (const DeviceArray<float12>&)output;
      device::mergePointNormal(c, n, o);           
    }

    Eigen::Vector3f rodrigues2(const Eigen::Matrix3f& matrix)
    {
      Eigen::JacobiSVD<Eigen::Matrix3f> svd(matrix, Eigen::ComputeFullV | Eigen::ComputeFullU);    
      Eigen::Matrix3f R = svd.matrixU() * svd.matrixV().transpose();

      double rx = R(2, 1) - R(1, 2);
      double ry = R(0, 2) - R(2, 0);
      double rz = R(1, 0) - R(0, 1);

      double s = sqrt((rx*rx + ry*ry + rz*rz)*0.25);
      double c = (R.trace() - 1) * 0.5;
      c = c > 1. ? 1. : c < -1. ? -1. : c;

      double theta = acos(c);

      if( s < 1e-5 )
      {
        double t;

        if( c > 0 )
          rx = ry = rz = 0;
        else
        {
          t = (R(0, 0) + 1)*0.5;
          rx = sqrt( std::max(t, 0.0) );
          t = (R(1, 1) + 1)*0.5;
          ry = sqrt( std::max(t, 0.0) ) * (R(0, 1) < 0 ? -1.0 : 1.0);
          t = (R(2, 2) + 1)*0.5;
          rz = sqrt( std::max(t, 0.0) ) * (R(0, 2) < 0 ? -1.0 : 1.0);

          if( fabs(rx) < fabs(ry) && fabs(rx) < fabs(rz) && (R(1, 2) > 0) != (ry*rz > 0) )
            rz = -rz;
          theta /= sqrt(rx*rx + ry*ry + rz*rz);
          rx *= theta;
          ry *= theta;
          rz *= theta;
        }
      }
      else
      {
        double vth = 1/(2*s);
        vth *= theta;
        rx *= vth; ry *= vth; rz *= vth;
      }
      return Eigen::Vector3d(rx, ry, rz).cast<float>();
    }
  }
}

//*************************************************************************************************************************************************************OPERATOR:start
// dkruglov start

bool 
pcl::gpu::KinfuTracker::getTransformICP(device::Intr intr, Vector3f cam_trans_global_prev, Matrix3frm cam_rot_global_prev, Vector3f& cam_trans_estimation, Matrix3frm& cam_rot_estimation)
{
  bool estimationSuccessful = true; // right now does not do anything. Use if determinant is below threshold	
  Matrix3frm cam_rot_global_prev_inv = cam_rot_global_prev.inverse ();  // Rprev.t();
  
  // GET CURRENT GLOBAL TRANSFORM
  Matrix3frm cam_rot_global_curr = cam_rot_global_prev;                 // transform to global coo for ith camera pose
  Vector3f cam_trans_global_curr = cam_trans_global_prev;
  
  Matrix3frm cam_rot_cumulative_increment = rmats_[0];                 // transform to global coo for ith camera pose
  Vector3f cam_trans_cumulative_increment;
  cam_trans_cumulative_increment(0);
  cam_trans_cumulative_increment(1);
  cam_trans_cumulative_increment(2);
   
  // CONVERT TO DEVICE TYPES 
  //LOCAL PREVIOUS TRANSFORM
  Mat33&  device_cam_rot_local_prev_inv = device_cast<Mat33> (cam_rot_global_prev_inv);

  float3& device_cam_trans_local_prev_tmp = device_cast<float3> (cam_trans_global_prev);
  float3 device_cam_trans_local_prev;
  device_cam_trans_local_prev.x = device_cam_trans_local_prev_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
  device_cam_trans_local_prev.y = device_cam_trans_local_prev_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
  device_cam_trans_local_prev.z = device_cam_trans_local_prev_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;

  {
    //ScopeTime time("icp-all");
    for (int level_index = LEVELS-1; level_index>=0; --level_index)
    {
      int iter_num = icp_iterations_[level_index];
      
      // current maps
      MapArr& vmap_curr = vmaps_curr_[level_index];
      MapArr& nmap_curr = nmaps_curr_[level_index];   
      
      // previous maps
      MapArr& vmap_g_prev = vmaps_g_prev_[level_index];
      MapArr& nmap_g_prev = nmaps_g_prev_[level_index];
      
      // We need to transform the maps from global to the local coordinates
      Mat33&  rotation_id = device_cast<Mat33> (rmats_[0]); // Identity Rotation Matrix. Because we only need translation
      float3 cube_origin = (getCyclicalBufferStructure ())->origin_metric;
      cube_origin.x = -cube_origin.x;
      cube_origin.y = -cube_origin.y;
      cube_origin.z = -cube_origin.z;

      MapArr& vmap_temp = vmap_g_prev;
      MapArr& nmap_temp = nmap_g_prev;
      device::tranformMaps (vmap_temp, nmap_temp, rotation_id, cube_origin, vmap_g_prev, nmap_g_prev); 

      for (int iter = 0; iter < iter_num; ++iter)
      {
        //CONVERT TO DEVICE TYPES
        // CURRENT LOCAL TRANSFORM
        Mat33&  device_cam_rot_local_curr = device_cast<Mat33> (cam_rot_global_curr);

        float3& device_cam_trans_local_curr_tmp = device_cast<float3> (cam_trans_global_curr);
        float3 device_cam_trans_local_curr; 
        device_cam_trans_local_curr.x = device_cam_trans_local_curr_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
        device_cam_trans_local_curr.y = device_cam_trans_local_curr_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
        device_cam_trans_local_curr.z = device_cam_trans_local_curr_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;

        Eigen::Matrix<double, 6, 6, Eigen::RowMajor> A;
        Eigen::Matrix<double, 6, 1> b;

        estimateCombined (device_cam_rot_local_curr, device_cam_trans_local_curr, vmap_curr, nmap_curr, device_cam_rot_local_prev_inv, device_cam_trans_local_prev, intr (level_index), 
                          vmap_g_prev, nmap_g_prev, distThres_, angleThres_, gbuf_, sumbuf_, A.data (), b.data ());

        //checking nullspace
        double det = A.determinant ();
		if ( fabs (det) < 1e-15 || pcl_isnan (det) )
        {
          if (pcl_isnan (det)) cout << "qnan" << endl;
          PCL_ERROR ("LOST...\n");
          break;
        }

        Eigen::Matrix<float, 6, 1> result = A.llt ().solve (b).cast<float>();

        float alpha = result (0);
        float beta  = result (1);
        float gamma = result (2);

        Eigen::Matrix3f cam_rot_incremental = (Eigen::Matrix3f)AngleAxisf (gamma, Vector3f::UnitZ ()) * AngleAxisf (beta, Vector3f::UnitY ()) * AngleAxisf (alpha, Vector3f::UnitX ());
        Vector3f cam_trans_incremental = result.tail<3> ();

        //compose
        cam_trans_global_curr = cam_rot_incremental * cam_trans_global_curr + cam_trans_incremental;
        cam_rot_global_curr = cam_rot_incremental * cam_rot_global_curr;
        
        cam_trans_cumulative_increment = cam_rot_incremental * cam_trans_cumulative_increment + cam_trans_incremental;
        cam_rot_cumulative_increment = cam_rot_incremental * cam_rot_cumulative_increment;
      }
    }
  }     
        ///WE were returning the new pose in global coordinates
	   	//cam_trans_estimation = cam_trans_global_curr;
	   	//cam_rot_estimation = cam_rot_global_curr;
	   	
	   	///Now we return the increment only, because the calculations on which increment to choose (FOVIS|ICP) are being done outside this operation.
	   	cam_trans_estimation = cam_trans_cumulative_increment;
	   	cam_rot_estimation = cam_rot_cumulative_increment;

	return (estimationSuccessful);
}

bool 
pcl::gpu::KinfuTracker::getTransformFOVIS(const DepthMap& depth_raw, const View& colors, device::Intr intr, Vector3f cam_trans_global_prev, Matrix3frm cam_rot_global_prev, Vector3f& cam_trans_estimation, Matrix3frm& cam_rot_estimation)
{	
	bool estimationSuccessful = true;	

    // Prepare data for FOVIS algorithm
	// 1 stage - download RGB data from GPU and convert it to grayscale
	PixelRGB* rgb_color_info = new PixelRGB[size_of_current_rgb_frame_grayscale];
	colors.download(rgb_color_info,colors.cols()*sizeof(PixelRGB));
	memset(current_rgb_frame_grayscale,0,size_of_current_rgb_frame_grayscale);
	for(int i=0; i<size_of_current_rgb_frame_grayscale; i++)
	{		
		current_rgb_frame_grayscale[i] = std::floor(0.2125 * rgb_color_info[i].r + 
			0.7154 * rgb_color_info[i].g + 
			0.0721 * rgb_color_info[i].b);
	}
	delete[] rgb_color_info;
	
	// 2 stage - download depth data from GPU and convert from unsigned to float
	unsigned short* gpu_current_frame_data = new unsigned short[size_of_current_detph_frame];	
	depth_raw.download(gpu_current_frame_data,depth_raw.cols() * sizeof(unsigned short));
	memset(current_detph_frame,0,size_of_current_detph_frame);
	// TODO: decrease time execution
	for (int i=0;i<size_of_current_detph_frame;i++)
	{
		if (gpu_current_frame_data[i]!=0)
			current_detph_frame[i] = static_cast<float>(gpu_current_frame_data[i]) * 0.001f;
		else
			current_detph_frame[i] = NAN;
	}
	delete[] gpu_current_frame_data;

	fovis_current_depth_frame->setDepthImage(current_detph_frame);
	
	// firstly we should invoke FOVIS routine for determining camera location
	visual_odometry_analyzer->processFrame(current_rgb_frame_grayscale,fovis_current_depth_frame);
		
	// get the integrated pose estimate.
    Eigen::Isometry3d cam_to_local = visual_odometry_analyzer->getPose();
	
    // get the motion estimate for this frame to the previous frame.
    Eigen::Isometry3d motion_estimate = visual_odometry_analyzer->getMotionEstimate();
	
	// Transform to global coo for ith camera pose
	Matrix3frm cam_rot_increment = motion_estimate.rotation().cast<float>();
	Vector3f   cam_trans_increment = motion_estimate.translation().cast<float>();
	Eigen::Vector3d rpy = motion_estimate.rotation().eulerAngles(0, 1, 2); // Only used to monitor if we got a bad pose estimation.

	// Compose
	///Now we return the increment only, because the calculations on which increment to choose (FOVIS|ICP) are being done outside this operation.
	cam_trans_estimation = cam_trans_increment;
	cam_rot_estimation = cam_rot_increment;
	
	if(
	    isnan(cam_trans_estimation(0)) ||
	    isnan(cam_trans_estimation(1)) ||
	    isnan(cam_trans_estimation(2)) ||
	    isnan(rpy(0)) ||
	    isnan(rpy(1)) ||
	    isnan(rpy(2))
	  )
	  {
	    estimationSuccessful = false;
	  }
	
	return (estimationSuccessful);
}

void 
pcl::gpu::KinfuTracker::getOffsetBetweenCameraPoses(Vector3f cam_trans_global_curr_one, Matrix3frm cam_rot_global_curr_one, Vector3f cam_trans_global_curr_two, Matrix3frm cam_rot_global_curr_two)
{
	
	//CALCULATING: start
	 Vector3f offsetTranslation;
     offsetTranslation(0) = cam_trans_global_curr_one(0) - cam_trans_global_curr_two(0);
     offsetTranslation(1) = cam_trans_global_curr_one(1) - cam_trans_global_curr_two(1);
     offsetTranslation(2) = cam_trans_global_curr_one(2) - cam_trans_global_curr_two(2);
     
     Matrix3frm offsetRotation;
     offsetRotation(0,0) = cam_rot_global_curr_one(0,0) - cam_rot_global_curr_two(0,0);
     offsetRotation(0,1) = cam_rot_global_curr_one(0,1) - cam_rot_global_curr_two(0,1);
     offsetRotation(0,2) = cam_rot_global_curr_one(0,2) - cam_rot_global_curr_two(0,2);
     offsetRotation(1,0) = cam_rot_global_curr_one(1,0) - cam_rot_global_curr_two(1,0);
     offsetRotation(1,1) = cam_rot_global_curr_one(1,1) - cam_rot_global_curr_two(1,1);
     offsetRotation(1,2) = cam_rot_global_curr_one(1,2) - cam_rot_global_curr_two(1,2);
     offsetRotation(2,0) = cam_rot_global_curr_one(2,0) - cam_rot_global_curr_two(2,0);
     offsetRotation(2,1) = cam_rot_global_curr_one(2,1) - cam_rot_global_curr_two(2,1);
     offsetRotation(2,2) = cam_rot_global_curr_one(2,2) - cam_rot_global_curr_two(2,2);
	
	//CALCULATING: end
	//PRINTING:start
	std::cout << "Trans ICP: " << cam_trans_global_curr_one(0) << " " << cam_trans_global_curr_one(1) << " " << cam_trans_global_curr_one(2) << std::endl;
    std::cout << "Trans FOV: " << cam_trans_global_curr_two(0) << " " << cam_trans_global_curr_two(1) << " " << cam_trans_global_curr_two(2) << std::endl;
    std::cout << "Trans OFF: " << offsetTranslation(0) << " " << offsetTranslation(1) << " " << offsetTranslation(2) << std::endl << std::endl;
    
    std::cout << "Rot ICP: " << cam_rot_global_curr_one(0,0) << " "
                                  << cam_rot_global_curr_one(0,1) << " "
                                  << cam_rot_global_curr_one(0,2) << " "
                                  << cam_rot_global_curr_one(1,0) << " "
                                  << cam_rot_global_curr_one(1,1) << " "
                                  << cam_rot_global_curr_one(1,2) << " "
                                  << cam_rot_global_curr_one(2,0) << " "
                                  << cam_rot_global_curr_one(2,1) << " "
                                  << cam_rot_global_curr_one(2,2) << std::endl;
                                  
    std::cout << "Rot FOV: " << cam_rot_global_curr_two(0,0) << " "
                                  << cam_rot_global_curr_two(0,1) << " "
                                  << cam_rot_global_curr_two(0,2) << " "
                                  << cam_rot_global_curr_two(1,0) << " "
                                  << cam_rot_global_curr_two(1,1) << " "
                                  << cam_rot_global_curr_two(1,2) << " "
                                  << cam_rot_global_curr_two(2,0) << " "
                                  << cam_rot_global_curr_two(2,1) << " "
                                  << cam_rot_global_curr_two(2,2) << std::endl;

    std::cout << "Rot OFF: " << offsetRotation(0,0) << " "
                                  << offsetRotation(0,1) << " "
                                  << offsetRotation(0,2) << " "
                                  << offsetRotation(1,0) << " "
                                  << offsetRotation(1,1) << " "
                                  << offsetRotation(1,2) << " "
                                  << offsetRotation(2,0) << " "
                                  << offsetRotation(2,1) << " "
                                  << offsetRotation(2,2) << std::endl<< std::endl;
     //PRINTING:end
}

bool 
pcl::gpu::KinfuTracker::operator() (const DepthMap& depth_raw, const View& colors)
{ 
   //std::cout << "Iteration " << global_time_ << std::endl;
  device::Intr intr (fx_, fy_, cx_, cy_);
  {
    device::bilateralFilter (depth_raw, depths_curr_[0]);

	if (max_icp_distance_ > 0)
		device::truncateDepth(depths_curr_[0], max_icp_distance_);

    for (int i = 1; i < LEVELS; ++i)
      device::pyrDown (depths_curr_[i-1], depths_curr_[i]);

    for (int i = 0; i < LEVELS; ++i)
    {
      device::createVMap (intr(i), depths_curr_[i], vmaps_curr_[i]);
      computeNormalsEigen (vmaps_curr_[i], nmaps_curr_[i]);
    }
    pcl::device::sync ();
  }
  
  // dkruglov start
//************************ INTEGRATION ****************************************//
	if (global_time_ == 0)
	{
		std::cout << "===========Start of Initial Integration (1)========" << std::endl;
		// TODO: check what will be in case of using FOVIS inition values?
		Matrix3frm initial_cam_rot = rmats_[0]; //  [Ri|ti] - pos of camera, i.e.
		Matrix3frm initial_cam_rot_inv = initial_cam_rot.inverse ();
		Vector3f   initial_cam_trans = tvecs_[0]; //  transform from camera to global coo space for (i-1)th camera pose

		Mat33&  device_initial_cam_rot = device_cast<Mat33> (initial_cam_rot);
		Mat33&  device_initial_cam_rot_inv = device_cast<Mat33> (initial_cam_rot_inv);
		float3& device_initial_cam_trans = device_cast<float3>(initial_cam_trans);
         
		float3 device_volume_size = device_cast<const float3>(tsdf_volume_->getSize());

		device::integrateTsdfVolume(depth_raw, intr, device_volume_size, device_initial_cam_rot_inv, device_initial_cam_trans, tsdf_volume_->getTsdfTruncDist(), tsdf_volume_->data(), getCyclicalBufferStructure (), depthRawScaled_);
    
		for (int i = 0; i < LEVELS; ++i)
			device::tranformMaps (vmaps_curr_[i], nmaps_curr_[i], device_initial_cam_rot, device_initial_cam_trans, vmaps_g_prev_[i], nmaps_g_prev_[i]);

		if(perform_last_scan_)
			finished_ = true;

		++global_time_;
		std::cout << "===========End of Initial Integration (1)========" << std::endl;
		return (true);
	}

   	Matrix3frm cam_rot_global_prev = rmats_[global_time_ - 1];   //  [Ri|ti] - pos of camera, i.e.
	Vector3f   cam_trans_global_prev = tvecs_[global_time_ - 1]; //  tranfrom from camera to global coo space for (i-1)th camera pose

    Matrix3frm cam_rot_global_curr;     // After deciding if we use FOVIS or ICP, the new transform (rotation) in global coordinates will be stored here.
    Vector3f   cam_trans_global_curr;   // After deciding if we use FOVIS or ICP, the new transform (translation) in global coordinates will be stored here.

    Vector3f   cam_trans_global_curr_ICP; //Sent to getTransformICP() by reference,  so the camera pose estimation (obtained with ICP) 
	Matrix3frm cam_rot_global_curr_ICP;   //in global coordinates will be stored here.
	
	Vector3f   cam_trans_global_curr_FOVIS; //Sent to getTransformICP() by reference,  so the camera pose estimation (obtained with FOVIS)
	Matrix3frm cam_rot_global_curr_FOVIS;   //in global coordinates will be stored here.

/////////////////////////Hybrid nature: based on Kintinuous paper
    bool gotTransformFOVIS = false;
    bool gotTransformICP = false;
    
    PCL_DEBUG("====================== Iteration %d ======================\n", global_time_);
    
    ///1) Get the estimation from both algorithms
    gotTransformFOVIS = getTransformFOVIS(depth_raw, colors, intr, cam_trans_global_prev, cam_rot_global_prev, cam_trans_global_curr_FOVIS, cam_rot_global_curr_FOVIS);
    gotTransformICP = getTransformICP(intr, cam_trans_global_prev, cam_rot_global_prev, cam_trans_global_curr_ICP, cam_rot_global_curr_ICP);

    ///Get the offset between the two algorithms. (Optional - its just a std::cout of these parameters)
    //getOffsetBetweenCameraPoses(cam_trans_global_curr_ICP, cam_rot_global_curr_ICP, cam_trans_global_curr_FOVIS, cam_rot_global_curr_FOVIS);
    
    ///2) The Euclidean norm is calculated inside the getTransformICP and getTransformFOVIS operations.
    ///   Because according to the paper the norm is obtained from the incremental translation component. 
    float tnormICP, tnormFOVIS;
    tnormICP = cam_trans_global_curr_ICP.norm();// The euclidean norm of the incremental translation component of the estimated camera pose (using ICP)
    tnormFOVIS = cam_trans_global_curr_FOVIS.norm();// The euclidean norm of the incremental translation component of the estimated camera pose (using ICP)
        
    ///4)Calculate the magnitude of the difference
    float mu = 0.03;                                    // Threshold used in the kintinuous paper.
    float normDifference = fabs(tnormFOVIS - tnormICP); // Calculate the absolute difference
    
   std::cout << "ICP norm: " << tnormICP << "; " 
              << "FOVIS norm: " << tnormFOVIS << "; "
              << "normDifference: " << normDifference << "; "
              << std::endl;
    
    if(normDifference > mu) // Push the FOVIS pose
    {
        PCL_ERROR("Pushed FOVIS pose\n\n"); // It's not an error. It is used for coloring
        cam_trans_global_curr = cam_rot_global_curr_FOVIS * cam_trans_global_prev + cam_trans_global_curr_FOVIS;
	    cam_rot_global_curr = cam_rot_global_curr_FOVIS * cam_rot_global_prev;
    }
    else //Push the ICP pose
    {
	    PCL_WARN("Pushed ICP pose\n\n"); // It's not a warning. It is used for coloring
	    cam_trans_global_curr = cam_rot_global_curr_ICP * cam_trans_global_prev + cam_trans_global_curr_ICP;
	    cam_rot_global_curr = cam_rot_global_curr_ICP * cam_rot_global_prev;
	}
       
      std::cout << "So the new one should be pushed\n Rotation:\n" << cam_rot_global_curr << std::endl << std::endl << "Translation:\n" << cam_trans_global_curr << std::endl; 
	  rmats_.push_back (cam_rot_global_curr);
	  tvecs_.push_back (cam_trans_global_curr);	
    
    ///ORIGINAL LOG MSG: USED in Raph's visualizer.
    //std::cout << "Rotation:\n" << cam_rot_global_curr << "\n Translation: \n"  << cam_trans_global_curr << std::endl << std::endl; 

//************************ INTEGRATION ****************************************//
//************************ CHECK FOR SHIFT ****************************************//
  ///check for shift
  bool has_shifted = cyclical_.checkForShift(tsdf_volume_, getCameraPose (), 0.6 * volume_size_, true, perform_last_scan_);

  if(has_shifted)
    PCL_WARN ("SHIFTING\n");
//************************ CHECK FOR SHIFT ****************************************//
//************************ GET COORDINATES IN LOCAL ****************************************//
  // get NEW local rotation 
  Matrix3frm cam_rot_local_curr_inv = cam_rot_global_curr.inverse ();
  Mat33&  device_cam_rot_local_curr_inv = device_cast<Mat33> (cam_rot_local_curr_inv);
  Mat33&  device_cam_rot_local_curr = device_cast<Mat33> (cam_rot_global_curr); 
  
  // get NEW local translation
  float3& device_cam_trans_local_curr_tmp = device_cast<float3> (cam_trans_global_curr);
  float3 device_cam_trans_local_curr;
  device_cam_trans_local_curr.x = device_cam_trans_local_curr_tmp.x - (getCyclicalBufferStructure ())->origin_metric.x;
  device_cam_trans_local_curr.y = device_cam_trans_local_curr_tmp.y - (getCyclicalBufferStructure ())->origin_metric.y;
  device_cam_trans_local_curr.z = device_cam_trans_local_curr_tmp.z - (getCyclicalBufferStructure ())->origin_metric.z;
//************************ GET COORDINATES IN LOCAL ****************************************//  
//************************ INTEGRATION CHECK ****************************************//  
  float rnorm = rodrigues2(cam_rot_global_curr.inverse() * cam_rot_global_prev).norm();
  float tnorm = (cam_trans_global_curr - cam_trans_global_prev).norm();    
  const float alpha = 1.f;
  bool integrate = (rnorm + alpha * tnorm)/2 >= integration_metric_threshold_;
//************************ INTEGRATION CHECK ****************************************//  
//************************ VOLUMETRIC INTEGRATION ****************************************//  
  float3 device_volume_size = device_cast<const float3> (tsdf_volume_->getSize());
  if (integrate)
  {
    integrateTsdfVolume (depth_raw, intr, device_volume_size, device_cam_rot_local_curr_inv, device_cam_trans_local_curr, tsdf_volume_->getTsdfTruncDist (), tsdf_volume_->data (), getCyclicalBufferStructure (), depthRawScaled_);
  }
//************************ VOLUMETRIC INTEGRATION ****************************************//  
//************************ RAYCASTING ****************************************//  
  {          
    raycast (intr, device_cam_rot_local_curr, device_cam_trans_local_curr, tsdf_volume_->getTsdfTruncDist (), device_volume_size, tsdf_volume_->data (), getCyclicalBufferStructure (), vmaps_g_prev_[0], nmaps_g_prev_[0]);
    
    // POST-PROCESSING: We need to transform the newly raycasted maps into the global space.
    Mat33&  rotation_id = device_cast<Mat33> (rmats_[0]); /// Identity Rotation Matrix. Because we only need translation
    float3 cube_origin = (getCyclicalBufferStructure ())->origin_metric;
        
    MapArr& vmap_temp = vmaps_g_prev_[0];
    MapArr& nmap_temp = nmaps_g_prev_[0];
    
    device::tranformMaps (vmap_temp, nmap_temp, rotation_id, cube_origin, vmaps_g_prev_[0], nmaps_g_prev_[0]);
    
    for (int i = 1; i < LEVELS; ++i)
    {
      resizeVMap (vmaps_g_prev_[i-1], vmaps_g_prev_[i]);
      resizeNMap (nmaps_g_prev_[i-1], nmaps_g_prev_[i]);
    }
    pcl::device::sync ();
  }
//************************ RAYCASTING ****************************************//  

  if(has_shifted && perform_last_scan_)
    extractAndMeshWorld ();

  ++global_time_;

  if (color_volume_)
  {
    const float3 device_volume_size = device_cast<const float3> (tsdf_volume_->getSize());
    device::Intr intr(fx_, fy_, cx_, cy_);

    Matrix3frm R_inv = rmats_.back().inverse();
    Vector3f   t     = tvecs_.back();
    
    Mat33&  device_Rcurr_inv = device_cast<Mat33> (R_inv);
    float3& device_tcurr = device_cast<float3> (t);
    
    device::updateColorVolume(intr, tsdf_volume_->getTsdfTruncDist(), device_Rcurr_inv, device_tcurr, vmaps_g_prev_[0], 
        colors, device_volume_size, color_volume_->data(), color_volume_->getMaxWeight());
  }
  return (true);

}
//*************************************************************************************************************************************************************OPERATOR:end

void pcl::gpu::KinfuTracker::Init_Fovis(fovis::CameraIntrinsicsParameters* camera_rgb_params,fovis::CameraIntrinsicsParameters* camera_depth_params)
{
	if (!visual_odometry_analyzer)
	{	
		fovis::Rectification* rect = new fovis::Rectification(*camera_rgb_params);
		fovis::VisualOdometryOptions* options = new fovis::VisualOdometryOptions(fovis::VisualOdometry::getDefaultOptions());
		
		size_of_current_rgb_frame_grayscale = camera_rgb_params->width*camera_rgb_params->height;
		current_rgb_frame_grayscale = new uint8_t[size_of_current_rgb_frame_grayscale];

		visual_odometry_analyzer=new fovis::VisualOdometry(rect, *options);
	}
	
	if (!fovis_current_depth_frame)
	{
		fovis_current_depth_frame = new fovis::DepthImage(*camera_depth_params, camera_depth_params->width, camera_depth_params->height);		
		size_of_current_detph_frame = camera_depth_params->width*camera_depth_params->height;
		current_detph_frame = new float[size_of_current_detph_frame];
	}

}

fovis::CameraIntrinsicsParameters* pcl::gpu::KinfuTracker::Get_Camera_Intrinsics(CAM_TYPE camera_type)
{
	fovis::CameraIntrinsicsParameters* cam_params = new fovis::CameraIntrinsicsParameters();
	switch (camera_type)
	{
		case DEPTH_CAM:	// return depth params
				{
					cam_params->width = cols_;
					cam_params->height = rows_;
					cam_params->fx = fx_;
					cam_params->fy = fy_;
					cam_params->cx = cx_;
					cam_params->cy = cy_;
					cout << "depth cam params:"<<
						"width "<< cam_params->width<<
						"height "<<cam_params->height<<
						" fx "<<cam_params->fx<<
						" fy "<<cam_params->fy<<
						" cx "<<cam_params->cx<<
						" cy "<<cam_params->cy<<endl;
				}
				break;
		case RGB_CAM:	// return rgb params
				{
					// FIXME
					int width = 640;
					int height = 480;
					cam_params->width = width;
					cam_params->height = height;
					cam_params->fx = 528.49404721;
					cam_params->fy = cam_params->fx;
					cam_params->cx = width / 2.0;
					cam_params->cy = height / 2.0;
					cout << "rgb cam params:"<<
						"width "<< cam_params->width<<
						"height "<<cam_params->height<<
						" fx "<<cam_params->fx<<
						" fy "<<cam_params->fy<<
						" cx "<<cam_params->cx<<
						" cy "<<cam_params->cy<<endl;
				}
				break;
		default:
			{
				//FIXME
			}
				break;

	};
	return cam_params;
}

// dkruglov end
