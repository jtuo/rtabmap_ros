/*
Copyright (c) 2010-2016, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <rtabmap_ros/OdometryROS.h>

#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <image_geometry/stereo_camera_model.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <cv_bridge/cv_bridge.h>

#include "rtabmap_ros/MsgConversion.h"

#include <rtabmap/core/util3d.h>
#include <rtabmap/core/util2d.h>
#include <rtabmap/utilite/ULogger.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/UStl.h>

using namespace rtabmap;

namespace rtabmap_ros
{

class RGBDOdometry : public rtabmap_ros::OdometryROS
{
public:
	RGBDOdometry() :
		OdometryROS(false, true, false),
		approxSync_(0),
		exactSync_(0),
		approxSync2_(0),
		exactSync2_(0),
		queueSize_(5)
	{
	}

	virtual ~RGBDOdometry()
	{
		rgbdSub_.shutdown();
		if(approxSync_)
		{
			delete approxSync_;
		}
		if(exactSync_)
		{
			delete exactSync_;
		}
		if(approxSync2_)
		{
			delete approxSync2_;
		}
		if(exactSync2_)
		{
			delete exactSync2_;
		}
	}

private:

	virtual void onOdomInit()
	{
		ros::NodeHandle & nh = getNodeHandle();
		ros::NodeHandle & pnh = getPrivateNodeHandle();

		int rgbdCameras = 1;
		bool approxSync = true;
		bool subscribeRGBD = false;
		pnh.param("approx_sync", approxSync, approxSync);
		pnh.param("queue_size", queueSize_, queueSize_);
		pnh.param("subscribe_rgbd", subscribeRGBD, subscribeRGBD);
		if(pnh.hasParam("depth_cameras"))
		{
			ROS_ERROR("\"depth_cameras\" parameter doesn't exist anymore. It is replaced by \"rgbd_cameras\" with the \"rgbd_image\" input topics. \"subscribe_rgbd\" should be also set to true.");
		}
		pnh.param("rgbd_cameras", rgbdCameras, rgbdCameras);
		if(rgbdCameras <= 0)
		{
			rgbdCameras = 1;
		}
		if(rgbdCameras > 2)
		{
			NODELET_FATAL("Only 2 cameras maximum supported yet.");
		}

		std::string subscribedTopicsMsg;
		if(subscribeRGBD)
		{
			if(rgbdCameras == 2)
			{
				rgbd_image1_sub_.subscribe(nh, "rgbd_image0", 1);
				rgbd_image2_sub_.subscribe(nh, "rgbd_image1", 1);

				if(approxSync)
				{
					approxSync2_ = new message_filters::Synchronizer<MyApproxSync2Policy>(
							MyApproxSync2Policy(queueSize_),
							rgbd_image1_sub_,
							rgbd_image2_sub_);
					approxSync2_->registerCallback(boost::bind(&RGBDOdometry::callbackRGBD2, this, _1, _2));
				}
				else
				{
					exactSync2_ = new message_filters::Synchronizer<MyExactSync2Policy>(
							MyExactSync2Policy(queueSize_),
							rgbd_image1_sub_,
							rgbd_image2_sub_);
					exactSync2_->registerCallback(boost::bind(&RGBDOdometry::callbackRGBD2, this, _1, _2));
				}
				subscribedTopicsMsg = uFormat("\n%s subscribed to (%s sync):\n   %s,\n   %s",
						getName().c_str(),
						approxSync?"approx":"exact",
						rgbd_image1_sub_.getTopic().c_str(),
						rgbd_image2_sub_.getTopic().c_str());
			}
			else
			{
				rgbdSub_ = nh.subscribe("rgbd_image", queueSize_, &RGBDOdometry::callbackRGBD, this);

				subscribedTopicsMsg =
						uFormat("\n%s subscribed to:\n   %s",
						getName().c_str(),
						rgbdSub_.getTopic().c_str());
			}
		}
		else
		{
			ros::NodeHandle rgb_nh(nh, "rgb");
			ros::NodeHandle depth_nh(nh, "depth");
			ros::NodeHandle rgb_pnh(pnh, "rgb");
			ros::NodeHandle depth_pnh(pnh, "depth");
			image_transport::ImageTransport rgb_it(rgb_nh);
			image_transport::ImageTransport depth_it(depth_nh);
			image_transport::TransportHints hintsRgb("raw", ros::TransportHints(), rgb_pnh);
			image_transport::TransportHints hintsDepth("raw", ros::TransportHints(), depth_pnh);

			image_mono_sub_.subscribe(rgb_it, rgb_nh.resolveName("image"), 1, hintsRgb);
			image_depth_sub_.subscribe(depth_it, depth_nh.resolveName("image"), 1, hintsDepth);
			info_sub_.subscribe(rgb_nh, "camera_info", 1);

			if(approxSync)
			{
				approxSync_ = new message_filters::Synchronizer<MyApproxSyncPolicy>(MyApproxSyncPolicy(queueSize_), image_mono_sub_, image_depth_sub_, info_sub_);
				approxSync_->registerCallback(boost::bind(&RGBDOdometry::callback, this, _1, _2, _3));
			}
			else
			{
				exactSync_ = new message_filters::Synchronizer<MyExactSyncPolicy>(MyExactSyncPolicy(queueSize_), image_mono_sub_, image_depth_sub_, info_sub_);
				exactSync_->registerCallback(boost::bind(&RGBDOdometry::callback, this, _1, _2, _3));
			}

			subscribedTopicsMsg = uFormat("\n%s subscribed to (%s sync):\n   %s,\n   %s,\n   %s",
					getName().c_str(),
					approxSync?"approx":"exact",
					image_mono_sub_.getTopic().c_str(),
					image_depth_sub_.getTopic().c_str(),
					info_sub_.getTopic().c_str());
		}
		this->startWarningThread(subscribedTopicsMsg, approxSync);
	}

	virtual void updateParameters(ParametersMap & parameters)
	{
		//make sure we are using Reg/Strategy=0
		ParametersMap::iterator iter = parameters.find(Parameters::kRegStrategy());
		if(iter != parameters.end() && iter->second.compare("0") != 0)
		{
			ROS_WARN("RGBD odometry works only with \"Reg/Strategy\"=0. Ignoring value %s.", iter->second.c_str());
		}
		uInsert(parameters, ParametersPair(Parameters::kRegStrategy(), "0"));
	}

	void commonCallback(
				const std::vector<cv_bridge::CvImageConstPtr> & rgbImages,
				const std::vector<cv_bridge::CvImageConstPtr> & depthImages,
				const std::vector<sensor_msgs::CameraInfo>& cameraInfos)
	{
		ROS_ASSERT(rgbImages.size() > 0 && rgbImages.size() == depthImages.size() && rgbImages.size() == cameraInfos.size());
		ros::Time higherStamp;
		int imageWidth = rgbImages[0]->image.cols;
		int imageHeight = rgbImages[0]->image.rows;
		int cameraCount = rgbImages.size();
		cv::Mat rgb;
		cv::Mat depth;
		pcl::PointCloud<pcl::PointXYZ> scanCloud;
		std::vector<CameraModel> cameraModels;
		for(unsigned int i=0; i<rgbImages.size(); ++i)
		{
			if(!(rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_8UC1) ==0 ||
				 rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::MONO8) ==0 ||
				 rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::MONO16) ==0 ||
				 rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::BGR8) == 0 ||
				 rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::RGB8) == 0) ||
				!(depthImages[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_16UC1) == 0 ||
				 depthImages[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1) == 0 ||
				 depthImages[i]->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0))
			{
				NODELET_ERROR("Input type must be image=mono8,mono16,rgb8,bgr8 and image_depth=32FC1,16UC1,mono16");
				return;
			}
			UASSERT_MSG(rgbImages[i]->image.cols == imageWidth && rgbImages[i]->image.rows == imageHeight,
					uFormat("imageWidth=%d vs %d imageHeight=%d vs %d",
							imageWidth,
							rgbImages[i]->image.cols,
							imageHeight,
							rgbImages[i]->image.rows).c_str());
			UASSERT_MSG(depthImages[i]->image.cols == imageWidth && depthImages[i]->image.rows == imageHeight,
					uFormat("imageWidth=%d vs %d imageHeight=%d vs %d",
							imageWidth,
							depthImages[i]->image.cols,
							imageHeight,
							depthImages[i]->image.rows).c_str());

			ros::Time stamp = rgbImages[i]->header.stamp>depthImages[i]->header.stamp?rgbImages[i]->header.stamp:depthImages[i]->header.stamp;

			if(i == 0)
			{
				higherStamp = stamp;
			}
			else if(stamp > higherStamp)
			{
				higherStamp = stamp;
			}

			Transform localTransform = getTransform(this->frameId(), rgbImages[i]->header.frame_id, stamp);
			if(localTransform.isNull())
			{
				return;
			}

			cv_bridge::CvImageConstPtr ptrImage = rgbImages[i];
			if(rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_8UC1) !=0 &&
			   rgbImages[i]->encoding.compare(sensor_msgs::image_encodings::MONO8) != 0)
			{
				ptrImage = cv_bridge::cvtColor(rgbImages[i], "mono8");
			}

			cv_bridge::CvImageConstPtr ptrDepth = depthImages[i];
			cv::Mat subDepth = ptrDepth->image;

			// initialize
			if(rgb.empty())
			{
				rgb = cv::Mat(imageHeight, imageWidth*cameraCount, ptrImage->image.type());
			}
			if(depth.empty())
			{
				depth = cv::Mat(imageHeight, imageWidth*cameraCount, subDepth.type());
			}

			if(ptrImage->image.type() == rgb.type())
			{
				ptrImage->image.copyTo(cv::Mat(rgb, cv::Rect(i*imageWidth, 0, imageWidth, imageHeight)));
			}
			else
			{
				NODELET_ERROR("Some RGB images are not the same type!");
				return;
			}

			if(subDepth.type() == depth.type())
			{
				subDepth.copyTo(cv::Mat(depth, cv::Rect(i*imageWidth, 0, imageWidth, imageHeight)));
			}
			else
			{
				NODELET_ERROR("Some Depth images are not the same type!");
				return;
			}

			cameraModels.push_back(rtabmap_ros::cameraModelFromROS(cameraInfos[i], localTransform));
		}

		rtabmap::SensorData data(
				rgb,
				depth,
				cameraModels,
				0,
				rtabmap_ros::timestampFromROS(higherStamp));

		this->processData(data, higherStamp);
	}

	void callback(
			const sensor_msgs::ImageConstPtr& image,
			const sensor_msgs::ImageConstPtr& depth,
			const sensor_msgs::CameraInfoConstPtr& cameraInfo)
	{
		callbackCalled();
		if(!this->isPaused())
		{
			std::vector<cv_bridge::CvImageConstPtr> imageMsgs(1);
			std::vector<cv_bridge::CvImageConstPtr> depthMsgs(1);
			std::vector<sensor_msgs::CameraInfo> infoMsgs;
			imageMsgs[0] = cv_bridge::toCvShare(image);
			depthMsgs[0] = cv_bridge::toCvShare(depth);
			infoMsgs.push_back(*cameraInfo);

			this->commonCallback(imageMsgs, depthMsgs, infoMsgs);
		}
	}

	void callbackRGBD(
			const rtabmap_ros::RGBDImageConstPtr& image)
	{
		callbackCalled();
		if(!this->isPaused())
		{
			std::vector<cv_bridge::CvImageConstPtr> imageMsgs(1);
			std::vector<cv_bridge::CvImageConstPtr> depthMsgs(1);
			std::vector<sensor_msgs::CameraInfo> infoMsgs;
			rtabmap_ros::toCvShare(image, imageMsgs[0], depthMsgs[0]);
			infoMsgs.push_back(image->cameraInfo);

			this->commonCallback(imageMsgs, depthMsgs, infoMsgs);
		}
	}

	void callbackRGBD2(
			const rtabmap_ros::RGBDImageConstPtr& image,
			const rtabmap_ros::RGBDImageConstPtr& image2)
	{
		callbackCalled();
		if(!this->isPaused())
		{
			std::vector<cv_bridge::CvImageConstPtr> imageMsgs(2);
			std::vector<cv_bridge::CvImageConstPtr> depthMsgs(2);
			std::vector<sensor_msgs::CameraInfo> infoMsgs;
			rtabmap_ros::toCvShare(image, imageMsgs[0], depthMsgs[0]);
			rtabmap_ros::toCvShare(image2, imageMsgs[1], depthMsgs[1]);
			infoMsgs.push_back(image->cameraInfo);
			infoMsgs.push_back(image2->cameraInfo);

			this->commonCallback(imageMsgs, depthMsgs, infoMsgs);
		}
	}

protected:
	virtual void flushCallbacks()
	{
		// flush callbacks
		if(approxSync_)
		{
			delete approxSync_;
			approxSync_ = new message_filters::Synchronizer<MyApproxSyncPolicy>(MyApproxSyncPolicy(queueSize_), image_mono_sub_, image_depth_sub_, info_sub_);
			approxSync_->registerCallback(boost::bind(&RGBDOdometry::callback, this, _1, _2, _3));
		}
		if(exactSync_)
		{
			delete exactSync_;
			exactSync_ = new message_filters::Synchronizer<MyExactSyncPolicy>(MyExactSyncPolicy(queueSize_), image_mono_sub_, image_depth_sub_, info_sub_);
			exactSync_->registerCallback(boost::bind(&RGBDOdometry::callback, this, _1, _2, _3));
		}
		if(approxSync2_)
		{
			delete approxSync2_;
			approxSync2_ = new message_filters::Synchronizer<MyApproxSync2Policy>(
					MyApproxSync2Policy(queueSize_),
					rgbd_image1_sub_,
					rgbd_image2_sub_);
			approxSync2_->registerCallback(boost::bind(&RGBDOdometry::callbackRGBD2, this, _1, _2));
		}
		if(exactSync2_)
		{
			delete exactSync2_;
			exactSync2_ = new message_filters::Synchronizer<MyExactSync2Policy>(
					MyExactSync2Policy(queueSize_),
					rgbd_image1_sub_,
					rgbd_image2_sub_);
			exactSync2_->registerCallback(boost::bind(&RGBDOdometry::callbackRGBD2, this, _1, _2));
		}
	}

private:
	image_transport::SubscriberFilter image_mono_sub_;
	image_transport::SubscriberFilter image_depth_sub_;
	message_filters::Subscriber<sensor_msgs::CameraInfo> info_sub_;

	ros::Subscriber rgbdSub_;
	message_filters::Subscriber<rtabmap_ros::RGBDImage> rgbd_image1_sub_;
	message_filters::Subscriber<rtabmap_ros::RGBDImage> rgbd_image2_sub_;

	typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::CameraInfo> MyApproxSyncPolicy;
	message_filters::Synchronizer<MyApproxSyncPolicy> * approxSync_;
	typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::CameraInfo> MyExactSyncPolicy;
	message_filters::Synchronizer<MyExactSyncPolicy> * exactSync_;
	typedef message_filters::sync_policies::ApproximateTime<rtabmap_ros::RGBDImage, rtabmap_ros::RGBDImage> MyApproxSync2Policy;
	message_filters::Synchronizer<MyApproxSync2Policy> * approxSync2_;
	typedef message_filters::sync_policies::ExactTime<rtabmap_ros::RGBDImage, rtabmap_ros::RGBDImage> MyExactSync2Policy;
	message_filters::Synchronizer<MyExactSync2Policy> * exactSync2_;
	int queueSize_;
};

PLUGINLIB_EXPORT_CLASS(rtabmap_ros::RGBDOdometry, nodelet::Nodelet);

}