#include "driverless/path_tracking.h"
#define __NAME__ "path_tracking"

PathTracking::PathTracking():
	AutoDriveBase(__NAME__),
	expect_speed_(1.0) //defult expect speed
{
	 
}

PathTracking::~PathTracking()
{
}

bool PathTracking::setExpectSpeed(float speed)
{
	expect_speed_ = fabs(speed);
}

bool PathTracking::init(ros::NodeHandle nh,ros::NodeHandle nh_private)
{
	nh_private.param<float>("foreSightDis_speedCoefficient", foreSightDis_speedCoefficient_,1.8);
	nh_private.param<float>("foreSightDis_latErrCoefficient", foreSightDis_latErrCoefficient_,-1.0);
	nh_private.param<float>("min_foresight_distance",min_foresight_distance_,5.0);
	nh_private.param<float>("max_side_accel",max_side_accel_,1.0);

	nh_private.param<float>("safety_distance",safety_distance_,4);
	nh_private.param<float>("timeout",timeout_,0.3);
    
	max_target_yaw_err_ = nh_private.param<float>("max_target_yaw_err",50.0)*M_PI/180.0;
	pub_nearest_index_  = nh.advertise<std_msgs::UInt32>("/driverless/nearest_index",1);
	pub_local_path_ = nh_private_.advertise<nav_msgs::Path>("/local_path",2);

	initDiagnosticPublisher(nh,__NAME__);

	is_ready_ = true;
		
	return true;
}

//启动跟踪线程
bool PathTracking::start()
{
	if(!is_ready_)
	{
		ROS_ERROR("[%s] System is not ready!",__NAME__);
		return false;
	}
	if(global_path_.size()==0)
	{
		ROS_ERROR("[%s] No global path!",__NAME__);
		return false;
	}
	if(global_path_.park_points.size()==0)
	{
		ROS_ERROR("[%s] No parking points!",__NAME__);
		return false;
	}

	if(vehicle_params_.validity == false)
	{
		ROS_ERROR("[%s] Vehicle parameters is invalid, please set them firstly.",__NAME__);
		return false;
	}

	is_running_ = true;
	std::thread t(&PathTracking::trackingThread,this);
	t.detach();
	return true;
}

//跟踪线程
void PathTracking::trackingThread()
{
	global_path_.pose_index = findNearestPoint(global_path_, vehicle_state_.getPose(LOCK)); 
	size_t nearest_index = global_path_.pose_index;

	if(nearest_index > global_path_.size() - 3)
	{
		ROS_ERROR("Remaind target path is too short! nearest_point_index:%lu", nearest_index);
		publishDiagnosticMsg(diagnostic_msgs::DiagnosticStatus::ERROR,"Remaind target path is too short!");
		is_running_ = false;
		return ;
	}

	ros::Rate loop_rate(30);
	
	size_t cnt =0;
	while(ros::ok() && is_running_ && !global_path_.finish())
	{
		float goal_speed = expect_speed_;
		const Pose pose = vehicle_state_.getPose(LOCK);
		const float vehicle_speed = vehicle_state_.getSpeed(LOCK);

		//横向偏差,左偏为负,右偏为正
		float lat_err = calculateDis2path(pose.x, pose.y, global_path_, nearest_index, &nearest_index);
		/**************PID-Kp*****************/
		global_path_.pose_index = nearest_index; //更新到基类公用变量
		//航向偏差,左偏为正,右偏为负
		float yaw_err = global_path_[nearest_index].yaw - pose.yaw;
		if(yaw_err > M_PI)       yaw_err -= 2*M_PI;
		else if(yaw_err < -M_PI) yaw_err += 2*M_PI;

		g_lateral_err_ = lat_err; //更新到基类公用变量
		g_yaw_err_ = yaw_err; //更新到基类公用变量
		
		disThreshold_ = foreSightDis_speedCoefficient_ * vehicle_speed 
		              + foreSightDis_latErrCoefficient_ * fabs(lat_err)
					  + min_foresight_distance_;
	
		if(disThreshold_ < min_foresight_distance_)
			disThreshold_  = min_foresight_distance_;
		size_t target_index = nearest_index+1;//跟踪目标点索引
	
		GpsPoint target_point = global_path_[target_index];
		//获取当前点到目标点的距离和航向
		std::pair<float, float> dis_yaw = getDisAndYaw(target_point, pose);

		//循环查找满足disThreshold_的目标点
		while(dis_yaw.first < disThreshold_)
		{
			target_point = global_path_[++target_index];
			dis_yaw = getDisAndYaw(target_point, pose);
		}

		float theta = dis_yaw.second - pose.yaw;
		float sin_theta = sin(theta);
        if(sin_theta == 0)
            continue;
		
		float turning_radius = (0.5 * dis_yaw.first)/sin(theta);

		float t_roadWheelAngle = generateRoadwheelAngleByRadius(vehicle_params_.wheel_base, turning_radius);
		
		t_roadWheelAngle = limitRoadwheelAngleBySpeed(t_roadWheelAngle, vehicle_speed);
	
		//float curvature_search_distance = disThreshold_ + 13; //曲率搜索距离
		float curvature_search_distance = vehicle_speed * vehicle_speed/(2 * 1);
		float max_curvature = maxCurvatureInRange(global_path_, nearest_index, curvature_search_distance);

		float max_speed_by_curve = generateMaxTolarateSpeedByCurvature(max_curvature, max_side_accel_);
		float max_speed_by_park =  limitSpeedByParkingPoint(max_speed_by_curve);
		float max_speed = max_speed_by_park;

		cmd_mutex_.lock();
		cmd_.validity = true;
		cmd_.speed = (goal_speed>max_speed) ? max_speed : goal_speed;
		cmd_.roadWheelAngle = t_roadWheelAngle;
		cmd_mutex_.unlock();
		
		publishNearestIndex();
		
		
		if((++cnt)%10==1)
		{
			ROS_INFO("max_v: expect:%.1f curve:%.1f  park:%.1f",expect_speed_, max_speed_by_curve, max_speed_by_park);
			ROS_INFO("set_v:%f m/s\t true_v:%f m/s",cmd_.speed ,vehicle_speed);
			ROS_INFO("yaw: %.2f\t targetYaw:%.2f", pose.yaw*180.0/M_PI , dis_yaw.second *180.0/M_PI);
			ROS_INFO("dis2target:%.2f  yaw_err:%.2f  lat_err:%.2f",dis_yaw.first,yaw_err_*180.0/M_PI,lat_err);
			ROS_INFO("disThreshold:%f   expect angle:%.2f",disThreshold_,t_roadWheelAngle);
			publishDiagnosticMsg(diagnostic_msgs::DiagnosticStatus::OK,"Running");
			publishLocalPath();
			ROS_INFO("near_index:%lu\t goal_index:%lu\t final_index:%lu",
				nearest_index,target_index, global_path_.final_index);
			printf("now:(%.2f,%.2f)\tnear:(%.2f,%.2f)\tgoal:(%.2f,%.2f)",
				pose.x, pose.y, global_path_[nearest_index].x, global_path_[nearest_index].y, target_point.x, target_point.y);
		}
		loop_rate.sleep();
	}
	
	publishDiagnosticMsg(diagnostic_msgs::DiagnosticStatus::OK,"Arrived at the destination.");
	ROS_INFO("[%s] path_tracking completed...", __NAME__);
	
	cmd_mutex_.lock();
	cmd_.speed = 0.0;
	cmd_.roadWheelAngle = 0.0;
	cmd_mutex_.unlock();
	
	is_running_ = false;
}

void PathTracking::publishNearestIndex()
{
	static std_msgs::UInt32 msg;
	if(pub_nearest_index_.getNumSubscribers())
	{
		msg.data = global_path_.pose_index;
		pub_nearest_index_.publish(msg);
	}
}

GpsPoint PathTracking::pointOffset(const GpsPoint& point,float offset)
{
	GpsPoint result = point;
	result.x =  offset * cos(point.yaw) + point.x;
	result.y = -offset * sin(point.yaw) + point.y;
	return result;
}

/*@brief 当前位置到停车点的距离
*/
float PathTracking::disToParkingPoint(const ParkingPoint& parking_point)
{
	if(global_path_.pose_index >=  parking_point.index) //当前位置已经超过停车点
		return 0;
	
	float dis1 = global_path_.resolution * (parking_point.index-global_path_.pose_index); //估计路程
	float dis2 = disBetweenPoints(global_path_[parking_point.index], global_path_[global_path_.pose_index]);//直线距离
	float dis2end = dis1 > dis2 ? dis1 : dis2;
	return dis2end;
	
}

/*@brief 根据停车点限制车辆速度
*/
float PathTracking::limitSpeedByParkingPoint(const float& speed,const float& acc)
{
	ParkingPoints& parking_points = global_path_.park_points;
	//初始化时parking_points至少有一个点(终点)
	//每到达一个停车点并完成停车后,更新下一个停车点
	//程序初始化时,应将停车点由近及远排序,并将位于当前位置之后的停车点复位
	if(parking_points.size() == 0)
	{
		ROS_DEBUG("[%s] No Parking Points, Speed prohibition!",__NAME__);
		return 0.0;
	}
	
	//无可用停车点,已经到达终点
	if(!parking_points.available())
	{
		ROS_DEBUG("[%s] No Next Parking Point, destination reached!",__NAME__);
		return 0.0;
	}
	
	while(parking_points.next().index < global_path_.pose_index)
	{//更新停车点
		++ parking_points.next_index;
		if(!parking_points.available())
			return 0.0;
	}
		
	ParkingPoint& parking_point = parking_points.next();
	
	if(parking_point.isParking) //正在停车中
	{
		//停车周期为0,到达终点
		if(parking_point.parkingDuration == 0.0)
			return 0.0;
		//停车超时,移除当前停车点,下次利用下一停车点限速
		if(ros::Time::now().toSec()-parking_point.parkingTime >= parking_point.parkingDuration)
		{
			ROS_INFO("parking overtime. parking point :%lu",parking_point.index);
			return speed;
		}
		//正在停车,时间未到
		return 0.0;
	}
	
	float dis2ParkingPoint = disToParkingPoint(parking_point);
	float maxSpeed = sqrt(2*acc*dis2ParkingPoint);
	//ROS_ERROR("parking_point :%d" , parking_point.index);
	//ROS_INFO("dis2ParkingPoint:%.2f\tmaxSpeed:%.2f",dis2ParkingPoint,maxSpeed);
	if(dis2ParkingPoint < 0.5)//到达停车点附近,速度置0,,防止抖动
	{
		parking_point.parkingTime = ros::Time::now().toSec();
		parking_point.isParking = true;
		ROS_INFO("[%s] start parking. point:%lu",__NAME__, parking_point.index);
		return 0.0;
	}
	return speed > maxSpeed ? maxSpeed : speed;
}

/*@brief 根据当前车速限制车辆前轮转角
 *@brief 算法已经根据路径曲率对车速进行了限制，
 *@brief 但可能出现转弯半径小于路径转弯半径,而导致侧向加速度超限的情况
 *@param angle 期望转角
 *@param speed 车速
 *@return 限制后的转角
 */
float PathTracking::limitRoadwheelAngleBySpeed(const float& angle, const float& speed)
{
	float min_steering_radius = speed*speed/max_side_accel_;
	
	if(min_steering_radius < 1.0)
		return angle;
	
	float max_angle = fabs(generateRoadwheelAngleByRadius(vehicle_params_.wheel_base, min_steering_radius));
	if(max_angle > vehicle_params_.max_roadwheel_angle)
	   max_angle = vehicle_params_.max_roadwheel_angle;
	//ROS_INFO("max_angle:%f\t angle:%f",max_angle,angle);
	return saturationEqual(angle,max_angle);
}

/*@brief 利用路径曲率限制最大车速
 *@brief 保证车辆侧向加速度在一定范围
 *@param curvature 路径曲率
 *@param max_accel 最大允许侧向加速度
 *
 *@return 最大车速 m/s
 */
float PathTracking::generateMaxTolarateSpeedByCurvature(const float& curvature, const float& max_accel)
{
	return sqrt(1.0/fabs(curvature)*max_accel);
}

/*@brief 利用路径曲率限制最大车速
 *@brief 保证车辆侧向加速度在一定范围
 *@param path_points        路径点
 *@param start_search_index 搜索起点
 *@param end_search_index   搜索终点
 *
 *@return 最大车速 km/h
 */
float PathTracking::generateMaxTolarateSpeedByCurvature(const std::vector<GpsPoint>& path_points,
											const size_t& start_search_index,
											const size_t& end_search_index,
											float max_side_accel)
{
	float max_cuvature = 0.0;
		
	for(size_t i=start_search_index; i < end_search_index; ++i)
	{
		if(fabs(path_points[i].curvature) > max_cuvature)
			max_cuvature = fabs(path_points[i].curvature);
	}
	return sqrt(1.0/max_cuvature*max_side_accel) *3.6;
}

/*@brief 利用当前前轮转角限制车速
 *@brief 保证车辆侧向加速度在一定范围
 *@param speed			期望车速
 *@param angle 			当前转角
 *
 *@return 最大车速 km/h
 */
float PathTracking::limitSpeedByCurrentRoadwheelAngle(float speed,float angle)
{
	float steering_radius = fabs(vehicle_params_.wheel_base/tan(angle*M_PI/180.0));
	float max_speed =  sqrt(steering_radius*max_side_accel_);
	
	return (speed>max_speed? max_speed: speed)*3.6;
}


void PathTracking::publishLocalPath()
{
    nav_msgs::Path path;
    path.header.stamp=ros::Time::now();
    path.header.frame_id="base_link";
	size_t startIndex = global_path_.pose_index;
	size_t endIndex   = global_path_.final_index;
	if(endIndex <= startIndex)
		return;

	Pose origin_point = vehicle_state_.getPose(LOCK);

	path.poses.reserve(endIndex-startIndex+1);
	
	std::cout << origin_point.x << "\t" << origin_point.y << "\t" << origin_point.yaw << std::endl;
	
	for(size_t i=startIndex; i<endIndex; ++i)
	{
		const auto& global_point = global_path_[i];
		std::pair<float,float> local_point = 
			global2local(origin_point.x,origin_point.y,origin_point.yaw, global_point.x,global_point.y);
		
		geometry_msgs::PoseStamped poseStamped;
        poseStamped.pose.position.x = local_point.first;
        poseStamped.pose.position.y = local_point.second;

        poseStamped.header.frame_id="base_link";
        path.poses.push_back(poseStamped);
	}
	pub_local_path_.publish(path);
}

