#include "emvs_node.h"

namespace emvs{

// TODO make sure git lfs is working for data files
//

EmvsNode::EmvsNode()
	: events_updated_(false),
      kf_dsi_(sensor_rows, sensor_cols, min_depth, max_depth, N_planes, fx, fy)
{
	events_sub_ = nh_.subscribe("dvs/events", 1000, &EmvsNode::eventCallback, this);
	ground_truth_sub_ = nh_.subscribe("optitrack/davis", 100, &EmvsNode::poseCallback, this);
	// camera_info_sub_ = nh_.subscribe("dvs/camera_info", 1, &EmvsNode::camerainfoCallback, this);

	pointcloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2> ("map_points", 1);
	map_points_.height = 1;

	latest_events_ = Mat::zeros(sensor_rows, sensor_cols, EVENT_IMAGE_TYPE);
	new_events_ = Mat::zeros(sensor_rows, sensor_cols, EVENT_IMAGE_TYPE);

	cv::namedWindow(OPENCV_WINDOW);
}

EmvsNode::~EmvsNode()
{
	cv::destroyWindow(OPENCV_WINDOW);
}

void EmvsNode::eventCallback(const dvs_msgs::EventArray& msg)
{
	// TODO add checks for new keyframe conditions while going thru event msg
	// Add all events to DSI
	for(int i=0; i<msg.events.size(); i++)
	{
	    new_events_.at<uchar>(static_cast<int>(msg.events[i].y), static_cast<int>(msg.events[i].x)) += 1;
	}
	showNormalizedImage(new_events_);
}

void EmvsNode::poseCallback(const geometry_msgs::PoseStamped& msg)
{
	bool new_kf = checkForNewKeyframe(msg);

	if(new_kf && events_updated_)
	{
		addDsiToMap();

		kf_pos_ << msg.pose.position.x, msg.pose.position.y, msg.pose.position.z;
		kf_quat_ << msg.pose.orientation.x, msg.pose.orientation.y, msg.pose.orientation.z, msg.pose.orientation.w;
		events_updated_ = false;
	}
	else if (cv::countNonZero(new_events_) > 0)
	{
		events_updated_ = true;

		latest_events_ = undistortImage(new_events_);
		addEventsToDsi(latest_events_); // TODO this takes time, so put it on a separate thread?
		new_events_.setTo(0);
	}
}

bool EmvsNode::checkForNewKeyframe(const geometry_msgs::PoseStamped& pose)
{
	// check for new keyframe (position dist threshold)
	Eigen::Vector3d cur_pos;
	cur_pos << pose.pose.position.x, pose.pose.position.y, pose.pose.position.z;
	Eigen::Vector4d cur_quat;
	cur_quat << pose.pose.orientation.x, pose.pose.orientation.y, pose.pose.orientation.z, pose.pose.orientation.w;
	double dist_from_kf = (cur_pos - kf_pos_).norm() + 0.1*(cur_quat - kf_quat_).norm();

	last_pose_ = pose;

	bool new_keyframe = false;
	if(dist_from_kf > new_kf_dist_thres_)
	{
		new_keyframe = true;
	}
	return new_keyframe;
}

Mat EmvsNode::undistortImage(const Mat input_image)
{
	cv::Mat output_image(sensor_rows, sensor_cols, EVENT_IMAGE_TYPE, cv::Scalar::all(0));
	cv::undistort(input_image, output_image, K_camera, D_camera);
	return output_image;
}

void EmvsNode::addEventsToDsi(const Mat& events)
{
	// Equations from Gallup, David, et al.
	//"Real-time plane-sweeping stereo with multiple sweeping directions." CVPR 2007

	// Find transform between current pose and keyframe
	// Find world->kf, world->camera. then do world->kf * inv(world->camera)
	Mat kf_M = makeTransformMatrix(kf_pos_[0], kf_pos_[1], kf_pos_[2],
								   kf_quat_[0], kf_quat_[1], kf_quat_[2], kf_quat_[3]);

	Mat cam_M = makeTransformMatrix(last_pose_.pose.position.x,
									last_pose_.pose.position.y,
									last_pose_.pose.position.z,
									last_pose_.pose.orientation.x,
									last_pose_.pose.orientation.y,
									last_pose_.pose.orientation.z,
									last_pose_.pose.orientation.w);

	Mat T_c2kf = kf_M * cam_M.inv();

	// Precompute some reused matrices
	Mat t(3, 1, DOUBLE_TYPE);
	T_c2kf.col(3).rowRange(0,3).copyTo(t);
	Mat n = (Mat_<double>(3,1) << 0, 0, -1);

	Mat R_transpose(3, 3, DOUBLE_TYPE);
	T_c2kf.rowRange(0,3).colRange(0,3).copyTo(R_transpose);
	Mat R_t_n = R_transpose*t*n.t();

	// For each plane, compute homography from image to plane, warp event image to plane and add to DSI
	Mat H_c2z;
	for(int i=0; i<kf_dsi_.N_planes_; i++)
	{
		double depth = kf_dsi_.getPlaneDepth(i);
		H_c2z = (K_camera * (R_transpose + R_t_n/depth)*K_camera.inv()).inv();

		cv::Mat event_img_warped;
		cv::warpPerspective(events, event_img_warped, H_c2z, cv::Size(sensor_cols, sensor_rows));

		kf_dsi_.addToDsi(event_img_warped, i);
	}
}

void EmvsNode::addDsiToMap()
{
	PointCloud new_points_kf_frame = kf_dsi_.getFiltered3dPoints();

	// Transform points to world frame TODO verify this
	Mat world_to_kf = makeTransformMatrix(kf_pos_[0], kf_pos_[1], kf_pos_[2],
							kf_quat_[0], kf_quat_[1], kf_quat_[2],kf_quat_[3]);
	Mat kf_to_world = world_to_kf.inv();
	Eigen::Matrix4f kf_to_world_tf;
	cv2eigen(kf_to_world, kf_to_world_tf);

	PointCloud new_points_world_frame;
  	pcl::transformPointCloud(new_points_kf_frame, new_points_world_frame, kf_to_world_tf);

	map_points_ += new_points_world_frame;

	// Publish map pointcloud
	sensor_msgs::PointCloud2 msg;
	pcl::toROSMsg(map_points_, msg);
	msg.header.frame_id = "world";
	pointcloud_pub_.publish(msg);

	kf_dsi_.resetDSI();
}

} // end namespace emvs

int main(int argc, char **argv)
{
	ros::init(argc, argv, "emvs_node");
	emvs::EmvsNode emvs_node;
	ros::spin();

	return 0;
}