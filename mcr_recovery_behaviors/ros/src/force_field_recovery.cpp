#include <force_field_recovery/force_field_recovery.h>

// Register this planner as a RecoveryBehavior plugin
PLUGINLIB_DECLARE_CLASS(force_field_recovery, ForceFieldRecovery, force_field_recovery::ForceFieldRecovery, nav_core::RecoveryBehavior)

using costmap_2d::NO_INFORMATION;

namespace force_field_recovery 
{
	ForceFieldRecovery::ForceFieldRecovery(): global_costmap_(NULL), local_costmap_(NULL), 
	tf_(NULL), initialized_(false) 
	{
		// empty constructor
	}

	void ForceFieldRecovery::initialize(std::string name, tf::TransformListener* tf,
		costmap_2d::Costmap2DROS* global_costmap, costmap_2d::Costmap2DROS* local_costmap)
	{
		if(!initialized_)
		{
			// initialization, this code will be executed only once
			
			// setting initial value for member variables
			detect_oscillation_is_enabled_ = false;
			previous_angle_ = 0.0;
			allowed_oscillations_ = 0;
			
			// receiving move_base variables and copying them over to class variables
			tf_ = tf;
			global_costmap_ = global_costmap;
			local_costmap_ = local_costmap;
			
			ros::NodeHandle private_nh("~/" + name);

			ROS_INFO("Initializing Force field recovery behavior...");
			
			// Getting values from parameter server and storing into class variables
			private_nh.param("velocity_scale_factor", velocity_scale_, 0.6);
			private_nh.param("obstacle_neighborhood", obstacle_neighborhood_, 0.6);
			private_nh.param("max_velocity", max_velocity_, 0.3);
			private_nh.param("timeout", timeout_, 3.0);
			private_nh.param("update_frequency", recovery_behavior_update_frequency_, 5.0);
			private_nh.param("oscillation_angular_tolerance",oscillation_angular_tolerance_, 2.8); //1.8
			private_nh.param("allowed_oscillations", allowed_oscillations_, 0);
			
			// Inform user about which parameters will be used for the recovery behavior
			ROS_INFO("Recovery behavior, using Force field velocity_scale parameter : %f"
			, (float) velocity_scale_);
			ROS_INFO("Recovery behavior, using Force field obstacle_neighborhood parameter : %f"
			, (float) obstacle_neighborhood_);
			ROS_INFO("Recovery behavior, using Force field max_velocity parameter : %f"
			, (float) max_velocity_);
			ROS_INFO("Recovery behavior, using Force field timeout parameter : %f"
			, (float) timeout_);
			ROS_INFO("Recovery behavior, using Force field recovery_behavior_update_frequency_ parameter : %f"
			, (float) recovery_behavior_update_frequency_);
			ROS_INFO("Recovery behavior, using Force field oscillation_angular_tolerance parameter : %f"
			, (float) oscillation_angular_tolerance_);
			ROS_INFO("Recovery behavior, using Force field allowed_oscillations parameter : %f"
			, (float) allowed_oscillations_);
			
			// set up cmd_vel publisher
			twist_pub_ = private_nh.advertise<geometry_msgs::Twist>("/cmd_vel_prio_medium", 1);
			
			// set up marker publishers
			vecinity_pub_ = private_nh.advertise<visualization_msgs::Marker>( "/force_field_obstacle_neighborhood", 1);
			
			// set up cloud publishers topic
			map_cloud_pub_ = private_nh.advertise<sensor_msgs::PointCloud2> ("/obstacle_cloud_map", 1);
			base_footprint_cloud_pub_ = private_nh.advertise<sensor_msgs::PointCloud2> ("/obstacle_cloud_base_link", 1);
			
			// setting initialized flag to true, preventing this code to be executed twice
			initialized_ = true;
		}
		else
		{
			ROS_ERROR("You should not call initialize twice on this object, doing nothing");
		}
	}

	void ForceFieldRecovery::runBehavior()
	{
		// preventing the use of this code before initialization
		if(!initialized_)
		{
			ROS_ERROR("This object must be initialized before runBehavior is called");
			return;
		}

		// checking if the received costmaps are empty, if so exit
		if(global_costmap_ == NULL || local_costmap_ == NULL)
		{
			ROS_ERROR("The costmaps passed to the ClearCostmapRecovery object cannot be NULL. Doing nothing.");
			return;
		}
		
		ROS_INFO("Running force field recovery behavior");
		
		// Moving base away from obstacles
		move_base_away(local_costmap_);
	}
	
	void ForceFieldRecovery::move_base_away(costmap_2d::Costmap2DROS* costmap_ros)
	{
		// this function moves the mobile base away from obstacles based on a costmap
		
		ros::Time start_time = ros::Time::now();
		
		bool no_obstacles_in_radius = false;
		bool timeout = false;
		int number_of_oscillations = 0;
		
		ros::Rate loop_rate(recovery_behavior_update_frequency_);
		
		//reset allowed_oscillations_ on each recovery behavior call
		allowed_oscillations_ = 0;
		
		// while certain time (timeout) or no obstacles inside radius do the loop 
		while(!timeout)
		{
			// 1. getting a snapshot of the costmap
			costmap_2d::Costmap2D* costmap_snapshot = costmap_ros->getCostmap();
			
			// 2. convert obstacles inside costmap into pointcloud
			pcl::PointCloud<pcl::PointXYZ> obstacle_cloud = costmap_to_pointcloud(costmap_snapshot);
			
			// 3. publish obstacle cloud
			sensor_msgs::PointCloud2 ros_obstacle_cloud = publish_cloud(obstacle_cloud, map_cloud_pub_, "/map");
			
			// 4. Change cloud to the reference frame of the robot
			pcl::PointCloud<pcl::PointXYZ> obstacle_cloud_bf = change_cloud_reference_frame(ros_obstacle_cloud, "/base_footprint");
			
			// 5. publish base link obstacle cloud
			publish_cloud(obstacle_cloud_bf, base_footprint_cloud_pub_, "/base_footprint");
			
			// 6. compute force field
			Eigen::Vector3f force_field = compute_force_field(obstacle_cloud_bf);
			
			// 7. move base in the direction of the force field
			double cmd_vel_x = force_field(0)*velocity_scale_;
			double cmd_vel_y = force_field(1)*velocity_scale_;
			ROS_INFO("Moving base into the direction of the force field x = %f, y = %f", (float) cmd_vel_x, (float) cmd_vel_y);
			move_base(cmd_vel_x, cmd_vel_y);
			
			// 8. Checking for stopping the loop conditions
			if(force_field(0) == 0 && force_field(1) == 0)
			{
				// force field = 0, 0 : means we are done and away from costmap obstacles
				
				no_obstacles_in_radius = true;
				break;
			}
			else if(detect_oscillations(force_field, number_of_oscillations)) // stop the loop if there is oscillations in the ff
			{
				// this means the robot is stucked in a small area, causing the force field
				// to go back and forward -> oscillating, therefore we need to stop the recovery
				
				ROS_INFO("Oscillation detected! , will stop now...");
				break;
			}
			else if(ros::Duration(ros::Time::now() - start_time).toSec() > timeout_)
			{
				//timeout, recovery behavior has been executed for timeout seconds and still no success, then abort
				ROS_WARN("Force field recovery behavior time out exceeded");
				timeout = true;
				break;
			}
			
			//9. publish markers (vecinity and force field vector) for visualization purposes
			publish_obstacle_neighborhood();
			
			//10. Control the frequency update for costmap update
			loop_rate.sleep();
		}
		
		// 11. Inform the user about the completition of the recovery behavior
		if(no_obstacles_in_radius)
		{
			ROS_INFO("Force field recovery succesfull");
		}
		
		// 12. stop the base
		move_base(0.0, 0.0);
	}
	
	pcl::PointCloud<pcl::PointXYZ> ForceFieldRecovery::costmap_to_pointcloud(const costmap_2d::Costmap2D* costmap)
	{
		
		// This function transforms occupied regions of a costmap, letal cost = 254 to 
		// pointcloud xyz coordinate
		
		// for storing and return the pointcloud
		pcl::PointCloud<pcl::PointXYZ> cloud;
		
		int x_size_ = costmap->getSizeInCellsX();
		int y_size_ = costmap->getSizeInCellsY();
		
		int current_cost = 0;
		
		// for transforming map to world coordinates
		double world_x;
		double world_y;
		
		for(int i = 0; i < x_size_ ; i++)
		{
			for(int j = 0; j < y_size_ ; j++)
			{
				// getting each cost
				current_cost = costmap->getCost(i, j);
				
				ROS_DEBUG("i, j = %d, %d : cost = %d ", i, j, current_cost);
				ROS_DEBUG("costmap cost [%d][%d] = %d", i, j, current_cost);
				
				// if cell is occupied by obstacle then add the centroid of the cell to the cloud
				if(current_cost == LETHAL_COST)
				{
					// get world coordinates of current occupied cell
					costmap->mapToWorld(i, j, world_x, world_y);
					
					ROS_DEBUG("point %d, %d = %f, %f ",i ,j , (float) world_x, (float) world_y);
					
					// adding occupied cell centroid coordinates to cloud
					cloud.push_back (pcl::PointXYZ (world_x, world_y, 0));
				}
			}
		}
		
		return cloud;
	}
	
	sensor_msgs::PointCloud2 ForceFieldRecovery::publish_cloud(pcl::PointCloud<pcl::PointXYZ> cloud, ros::Publisher &cloud_pub, std::string frame_id)
	{
		// This function receives a pcl pointcloud, transforms into ros pointcloud and then publishes the cloud
		
		ROS_DEBUG("Publishing obstacle cloud");
		
		// Print points of the cloud in terminal
		pcl::PointCloud<pcl::PointXYZ>::const_iterator cloud_iterator = cloud.begin();
		
		int numPoints = 0;
		
		while (cloud_iterator != cloud.end())
		{
			ROS_DEBUG("cloud [%d] = %f, %f, %f ", numPoints, (float)cloud_iterator->x, (float)cloud_iterator->y, (float)cloud_iterator->z);
			++cloud_iterator;
			numPoints++;
		}
		
		ROS_DEBUG("total number of points in the cloud = %d", numPoints);
		
		// Creating a pointcloud2 data type
		pcl::PCLPointCloud2 cloud2;
		
		// Converting normal cloud to pointcloud2 data type
		pcl::toPCLPointCloud2(cloud, cloud2);
		
		// declaring a ros pointcloud data type
		sensor_msgs::PointCloud2 ros_cloud;
		
		// converting pointcloud2 to ros pointcloud
		pcl_conversions::fromPCL(cloud2, ros_cloud);
		
		// assigning a frame to ros cloud
		ros_cloud.header.frame_id = frame_id;
		
		// publish the cloud
		cloud_pub.publish(ros_cloud);
		
		// returning the cloud, it could be useful for other components
		return ros_cloud;
	}
	
	pcl::PointCloud<pcl::PointXYZ> ForceFieldRecovery::change_cloud_reference_frame(sensor_msgs::PointCloud2 ros_cloud, std::string target_reference_frame)
	{
		// This function receives a ros cloud (with an associated tf) and tranforms 
		// all the points to another reference frame (target_reference_frame)
		
		// declaring the target ros pcl data type
		sensor_msgs::PointCloud2 target_ros_pointcloud;
		
		// changing pointcloud reference frame
   
		// declaring normal PCL clouds (not ros related)
		pcl::PointCloud<pcl::PointXYZ> cloud_in;
		pcl::PointCloud<pcl::PointXYZ> cloud_trans;
		
		// convert from rospcl to pcl
		pcl::fromROSMsg(ros_cloud, cloud_in);
		
		// STEP 1 Convert xb3 message to center_bumper frame (i think it is better this way)
		tf::StampedTransform transform;
		try
		{
			tf_->lookupTransform(target_reference_frame, ros_cloud.header.frame_id, ros::Time(0), transform);
		}
		catch (tf::TransformException ex)
		{
			ROS_ERROR("%s",ex.what());
		}
		
		// Transform point cloud
		pcl_ros::transformPointCloud (cloud_in, cloud_trans, transform);  
		
		return cloud_trans;
	}
	
	Eigen::Vector3f ForceFieldRecovery::compute_force_field(pcl::PointCloud<pcl::PointXYZ> cloud)
	{
		// This function receives a cloud and returns the negative of the resultant
		// assuming that all points in the cloud are vectors
		
		Eigen::Vector3f force_vector(0, 0, 0);
		
		pcl::PointCloud<pcl::PointXYZ>::const_iterator cloud_iterator = cloud.begin();
		int numPoints = 0;

		while (cloud_iterator != cloud.end())
		{
			Eigen::Vector3f each_point(cloud_iterator->x, cloud_iterator->y, 0);
			
			ROS_DEBUG("Norm of the point : %f", each_point.norm());
			
			if (each_point.norm() < obstacle_neighborhood_)
			{
				force_vector -= each_point;
				numPoints++;
			}
		
			++cloud_iterator;
		}

		if (numPoints == 0) 
		{
			// Cloud is empty
			
			return Eigen::Vector3f(0, 0, 0);
		}
		
		force_vector.normalize();
		force_vector = force_vector * 1.0;
		
		ROS_DEBUG("Force vector = (%f, %f)", (float) force_vector(0), (float) force_vector(1));
		
		return force_vector;
	}
	
	bool ForceFieldRecovery::detect_oscillations(Eigen::Vector3f force_field, int &number_of_oscillations)
	{
		// This function detects oscillations in the force field and stops the recovery
		
		double current_angle = 0.0;
		double angle_difference = 0.0;
		
		// do not check for oscillations the first time, since there is no previous force to compare with
		if(detect_oscillation_is_enabled_)
		{
			// get the new force field angle
			current_angle = atan2(force_field(1) , force_field(0));
			
			ROS_INFO("previous angle : %f", (float) previous_angle_);
			ROS_INFO("current angle : %f", (float) current_angle);

			// compare the angles
			angle_difference = atan2(sin(current_angle - previous_angle_), cos(current_angle - previous_angle_));
			
			ROS_INFO("angle_difference = %f", (float) angle_difference);
			
			// detect if the force field angle has an abrupt angular change
			if(fabs(angle_difference) > oscillation_angular_tolerance_)
			{
				ROS_INFO("A big change in direction of the force field was detected");
				number_of_oscillations ++;
			}

			// making backup of the previous force field angle
			previous_angle_ = current_angle;
		}
		else
		{
			// compute angle of the first force field
			previous_angle_ = atan2(force_field(1) , force_field(0));

			// starting from second time, check for oscillations
			detect_oscillation_is_enabled_ = true;
		}
		
		if(number_of_oscillations > allowed_oscillations_)
		{
			// more than "n" allowed oscillations have been detected on the force field
			return true;
		}
		else
		{
			// no more than "n" allowed oscillations were detected in the force field so far
			return false;
		}
	}
	
	void ForceFieldRecovery::move_base(double x, double y)
	{
		// This function receives x and y velocity and publishes to cmd_vel topic to move the mobile base
		
		geometry_msgs::Twist twist_msg;
		
		// clamping x and y to maximum speed value
		if(x > max_velocity_) x = max_velocity_;
		else if(x < -max_velocity_) x = -max_velocity_;
		
		if(y > max_velocity_) y = max_velocity_;
		else if(y < -max_velocity_) y = -max_velocity_;
		
		twist_msg.linear.x = x;
		twist_msg.linear.y = y;
		twist_msg.linear.z = 0.0;
		twist_msg.angular.x = 0.0;
		twist_msg.angular.y = 0.0;
		twist_msg.angular.z = 0.0;
		
		twist_pub_.publish(twist_msg);
	}
	
	//void ForceFieldRecovery::ff_as_marker(Eigen::Vector3f force_field)
	//{
		// This function is for visualization of the force_field vector in rviz
		
	//}
	
	void ForceFieldRecovery::publish_obstacle_neighborhood()
	{
		ROS_INFO("Publishing obstacle neighbourhood...");
		// This function is for visualization of the vecinity area in rviz
		
		// declaring a marker object
		visualization_msgs::Marker marker;
		
		// filling the required data for the marker
		marker.header.frame_id = "base_footprint";
		marker.header.stamp = ros::Time::now();
		marker.ns = "force_field_visualization";
		marker.id = 0;
		marker.type = visualization_msgs::Marker::CYLINDER;
		//marker.type = visualization_msgs::Marker::CUBE;
		marker.action = visualization_msgs::Marker::ADD;
		
		marker.pose.position.x = 0;
		marker.pose.position.y = 0;
		marker.pose.position.z = 0;
		
		marker.pose.orientation.x = 0.0;
		marker.pose.orientation.y = 0.0;
		marker.pose.orientation.z = 0.0;
		marker.pose.orientation.w = 1.0;
		
		//duration of the marker : forever
		marker.lifetime = ros::Duration(0);
		
		marker.scale.x = obstacle_neighborhood_ * 2;
		marker.scale.y = obstacle_neighborhood_ * 2;
		marker.scale.z = 0.1;
		
		marker.color.a = 0.5; //alpha (transparency level)
		marker.color.r = 0.0;
		marker.color.g = 1.0;
		marker.color.b = 0.0;
		
		// publish the vecinity as cylinder marker
		vecinity_pub_.publish(marker);
		
	}
};