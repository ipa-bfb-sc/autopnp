#include <ipa_room_exploration/grid_point_explorator.h>

// Constructor
GridPointExplorator::GridPointExplorator(int grid_line_length)
{
	grid_line_length_ = grid_line_length;
}

// Function to create a static pose series that has the goal to inspect the complete floor of the given room.
// This is done in the following steps:
//		I. It lays a grid over the given map, with a line_size defined by the constructor/set-function. All intersection points
//		   that are not laying on an obstacle, or near to one, become one point of the path. The grid starts at the upper left
//		   point that is reachable.
//		II. It plans an optimal series of the previously found points by solving a Traveling-Salesman-Problem (TSP). This produces
//			a path that covers all nodes and ends at the node where the path started. depending on this series the angle of the
//			Poses are computed, by calculating a vector from the old node to the next and using the angle of this with the x-axis
//			as angle for the Poses.
void GridPointExplorator::getExplorationPath(const cv::Mat& room_map, std::vector<geometry_msgs::Pose2D>& path, const float map_resolution,
		const cv::Point starting_position, const cv::Point2d map_origin, const bool plan_for_footprint, const Eigen::Matrix<float, 2, 1> robot_to_fov_vector)
{
	// *********************** I. Find the main directions of the map and rotate it in this manner. ***********************
	// rotate map
	cv::Mat R;
	cv::Rect bbox;
	cv::Mat rotated_room_map;
	RoomRotator room_rotation;
	room_rotation.computeRoomRotationMatrix(room_map, R, bbox, map_resolution);
	room_rotation.rotateRoom(room_map, rotated_room_map, R, bbox);

	// transform starting position
	std::vector<cv::Point> starting_point_vector(1, starting_position); // opencv syntax
	cv::transform(starting_point_vector, starting_point_vector, R);
	cv::Point rotated_starting_position = starting_point_vector[0]; // Point that keeps track of the last point after the boustrophedon path in each cell

	// compute min/max room coordinates
	cv::Point min_room(1000000, 1000000), max_room(0, 0);
	for (int v=0; v<rotated_room_map.rows; ++v)
	{
		for (int u=0; u<rotated_room_map.cols; ++u)
		{
			if (rotated_room_map.at<uchar>(v,u)==255)
			{
				min_room.x = std::min(min_room.x, u);
				min_room.y = std::min(min_room.y, v);
				max_room.x = std::max(max_room.x, u);
				max_room.y = std::max(max_room.y, v);
			}
		}
	}

	//******************* II. Get grid points *************************
	// vector to store all found points
	std::vector<cv::Point> grid_points;

	// iterate trough the columns and rows with stepsize as the grid_size, start at the upper left point
	//		the given min/max-Polygon stores the min/max coordinates in two points: the first showing the min and the other
	//		showing the max coordinates
	std::cout << "size of one grid line: " << grid_line_length_ << std::endl;
	// todo: create grid in external class - it is the same in all approaches
	// todo: if first/last row or column in grid has accessible areas but center is inaccessible, create a node in the accessible area
	for(unsigned int v=min_room.y; v<=max_room.y; v+=grid_line_length_)
	{
		for(unsigned int u=min_room.x; u<=max_room.x; u+=grid_line_length_)
		{
			// check if point is in the free space
			if(rotated_room_map.at<unsigned char>(v, u) == 255)
			{
				grid_points.push_back(cv::Point(u, v));
			}
		}
	}

	// print results
//	cv::Mat point_map = rotated_room_map.clone();
//	for(std::vector<cv::Point>::iterator point = grid_points.begin(); point != grid_points.end(); ++point)
//		cv::circle(point_map, *point, 2, cv::Scalar(127), CV_FILLED);
//	cv::imshow("grid", point_map);
//	cv::waitKey();

	//******************* II. Plan a path trough the found points *************************
	// find the index of the point, which is closest to the starting position
	int min_index = 0;
	double min_distance = 1e10;

	for(std::vector<cv::Point>::iterator point = grid_points.begin(); point != grid_points.end(); ++point)
	{
		double distance = cv::norm(rotated_starting_position - *point);

		if(distance < min_distance)
		{
			min_distance = distance;
			min_index = point - grid_points.begin();
		}
	}


	// solve the Traveling Salesman Problem
	std::cout << "Finding optimal order of the " << grid_points.size() << " found points. Start-index: " << min_index << std::endl;
	ConcordeTSPSolver tsp_solver;
	const double map_downsampling_factor = 0.25;
	std::vector<int> optimal_order = tsp_solver.solveConcordeTSP(rotated_room_map, grid_points, map_downsampling_factor, 0.0, map_resolution, min_index, 0);

	// rearrange the found points in the optimal order and convert them to the right format
	std::vector<cv::Point> fov_middlepoint_path(optimal_order.size());
	for(unsigned int point_index = 0; point_index < optimal_order.size(); ++point_index)
		fov_middlepoint_path[point_index] = grid_points[optimal_order[point_index]];

	// transform the calculated path back to the originally rotated map and create poses with an angle
	std::vector<geometry_msgs::Pose2D> path_fov_poses;
	room_rotation.transformPathBackToOriginalRotation(fov_middlepoint_path, path_fov_poses, R);

//	for(unsigned int point_index = 0; point_index < fov_middlepoint_path.size(); ++point_index)
//	{
//		// get the vector from the current point to the next point
//		cv::Point current_point = grid_points[optimal_order[point_index]];
//		cv::Point next_point = grid_points[optimal_order[(point_index+1)%(optimal_order.size())]];
//		cv::Point vector = next_point - current_point;
//
//		float angle = std::atan2(vector.y, vector.x);//std::acos(quotient);
//
//		// add the next navigation goal to the path
//		geometry_msgs::Pose2D navigation_goal;
//		navigation_goal.x = (current_point.x * map_resolution) + map_origin.x; // coordinate systems are rotated to each other
//		navigation_goal.y = (current_point.y * map_resolution) + map_origin.y;
//		navigation_goal.theta = angle;
//
//		path.push_back(navigation_goal);
//	}

	// if the path should be planned for the robot footprint create the path and return here
	if(plan_for_footprint == true)
	{
		for(std::vector<geometry_msgs::Pose2D>::iterator pose=path_fov_poses.begin(); pose != path_fov_poses.end(); ++pose)
		{
			geometry_msgs::Pose2D current_pose;
			current_pose.x = (pose->x * map_resolution) + map_origin.x;
			current_pose.y = (pose->y * map_resolution) + map_origin.y;
			current_pose.theta = pose->theta;
			path.push_back(current_pose);
		}
		return;
	}

	// *********************** III. Get the robot path out of the fov path. ***********************
	// go trough all computed fov poses and compute the corresponding robot pose
	ROS_INFO("Starting to map from field of view pose to robot pose");
	mapPath(room_map, path, path_fov_poses, robot_to_fov_vector, map_resolution, map_origin, starting_position);
}
