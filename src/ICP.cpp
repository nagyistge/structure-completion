#include "ICP.h"

#include <assert.h>
#include <Eigen/Geometry>
#include <Eigen/LU> 
#include <Eigen/SVD>

namespace ICP {
	ANNkd_tree* create_kd_tree(
		const Eigen::MatrixXd &_points,
		ANNpointArray &_ann_points)
	{
		assert(_points.rows() == 3);
		assert(_points.cols() > 0);

		int dimension = 3;
		int num_points = static_cast<int>(_points.cols());
		ANNkd_tree* ann_kd_tree;

		_ann_points = annAllocPts(num_points, dimension);	// allocate data points

		// read data points
		for (int point_index = 0; point_index < num_points; point_index++)
		{
			for (int i = 0; i < dimension; i++)
				_ann_points[point_index][i] = _points.col(point_index)[i];
		}

		ann_kd_tree = new ANNkd_tree(		// build search structure
			_ann_points,					// the data points
			num_points,						// number of points
			dimension);						// dimension of space

		return ann_kd_tree;
	}

	template<typename T>
	void get_closest_points(
		ANNkd_tree *_data_ann_kd_tree,
		const Eigen::MatrixXd &_query_points,
		const Eigen::MatrixBase<T> &_data_values,
		Eigen::MatrixBase<T> &_closest_data_values)
	{
		assert(_query_points.rows() == 3);
		assert(_query_points.cols() > 0);
		assert(_data_values.cols() > 0);

		int num_data = static_cast<int>(_data_values.rows());
		int dimension = static_cast<int>(_query_points.cols());

		_closest_data_values = Eigen::MatrixBase<T>::Zero(num_data, dimension);
		assert(_data_ann_kd_tree->nPoints() == _data_values.cols());

		ANNpoint q = annAllocPt(3);
		ANNidxArray nn_idx = new ANNidx[1];
		ANNdistArray dd = new ANNdist[1];

		for (int point_index = 0; point_index < _query_points.cols(); point_index++)
		{
			for (unsigned int i = 0; i < 3; ++i)
				q[i] = _query_points.col(point_index)(i);

			_data_ann_kd_tree->annkSearch(q, 1, nn_idx, dd);
			int closest_Y_point_index = nn_idx[0];

			_closest_data_values.col(point_index) = _data_values.col(closest_Y_point_index);
		}

		// Deallocate ANN.
		annDeallocPt(q);
		delete[] nn_idx;
		delete[] dd;
	}

	double compute_rigid_transformation(const Eigen::MatrixXd &_X, const Eigen::MatrixXd &_Y,
		Eigen::Matrix3d &_rotation_mat, Eigen::Vector3d &_translation_vec)
	{
		assert(_X.rows() == 3);
		assert(_X.cols() > 0);

		assert(_Y.rows() == 3);
		assert(_Y.cols() > 0);

		Eigen::MatrixXd::Index X_num_points = _X.cols();
		Eigen::MatrixXd::Index Y_num_points = _Y.cols();

		ANNpointArray X_ann_points;
		ANNkd_tree* X_ann_kd_tree = create_kd_tree(_X, X_ann_points);

		ANNpointArray Y_ann_points;
		ANNkd_tree* Y_ann_kd_tree = create_kd_tree(_Y, Y_ann_points);
		
		Eigen::MatrixXd closest_X; 
		Eigen::MatrixXd closest_Y;

		get_closest_points(X_ann_kd_tree, _Y, _X, closest_X);
		get_closest_points(Y_ann_kd_tree, _X, _Y, closest_Y);

		annDeallocPts(X_ann_points); delete X_ann_kd_tree;
		annDeallocPts(Y_ann_points); delete Y_ann_kd_tree;

		assert(closest_X.cols() == Y_num_points);
		assert(closest_Y.cols() == X_num_points);

		Eigen::MatrixXd::Index n = X_num_points + Y_num_points;
		Eigen::MatrixXd all_X(3, n);
		Eigen::MatrixXd all_Y(3, n);

		all_X << _X, closest_X;
		all_Y << closest_Y, _Y;

		assert(all_X.cols() == n);
		assert(all_Y.cols() == n);

		Eigen::MatrixXd diff = all_X - all_Y;
		Eigen::RowVectorXd squared_dists = (diff.array() * diff.array()).colwise().sum();
		// hausdorff distance.
		//double prev_error = std::sqrt(squared_dists.maxCoeff());

		Eigen::Vector3d all_X_mean = all_X.rowwise().mean();
		Eigen::Vector3d all_Y_mean = all_Y.rowwise().mean();

		Eigen::MatrixXd centered_all_X = all_X.colwise() - all_X_mean;
		Eigen::MatrixXd centered_all_Y = all_Y.colwise() - all_Y_mean;

		Eigen::MatrixXd S = centered_all_X * centered_all_Y.transpose();
		assert(S.rows() == 3);
		assert(S.cols() == 3);

		Eigen::JacobiSVD<Eigen::MatrixXd> svd(S, Eigen::ComputeFullU | Eigen::ComputeFullV);

		Eigen::Matrix3d det_mat = Eigen::Matrix3d::Identity();
		det_mat(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant();

		_rotation_mat = svd.matrixV() * det_mat * svd.matrixU().transpose();
		_translation_vec = all_Y_mean - _rotation_mat * all_X_mean;

		diff = ((_rotation_mat * all_X).colwise() + _translation_vec) - all_Y;
		squared_dists = (diff.array() * diff.array()).colwise().sum();
		// hausdorff distance.
		double error = std::sqrt(squared_dists.maxCoeff());

		return error;
	}

	double run_iterative_closest_points(Eigen::MatrixXd &_X, Eigen::MatrixXd &_Y,
		Eigen::Matrix3d &_rotation_mat, Eigen::Vector3d &_translation_vec)
	{
		_rotation_mat = Eigen::Matrix3d::Identity();
		_translation_vec = Eigen::Vector3d::Zero();
		double prev_error = std::numeric_limits<double>::max();

		const unsigned int max_num_iterations = MAX_BBOX_ORIENTATION_NUM_ITERATIONS;
		const double min_angle_difference = static_cast<double>(MIN_BBOX_ORIENTATION_ANGLE_DIFFERENCE) / 180.0 * M_PI;
		const double min_translation = MIN_BBOX_ORIENTATION_TRANSLATION;

		for (unsigned int iteration = 0; iteration < max_num_iterations; ++iteration)
		{
			Eigen::Matrix3d new_rotation_mat;
			Eigen::Vector3d new_translation_vec;
			double error = compute_rigid_transformation(_X, _Y, new_rotation_mat, new_translation_vec);

			// NOTE:
			// Continue only when the error value is decreasing.
			if (error > prev_error)	return prev_error;
			prev_error = error;

			Eigen::AngleAxisd rotation_angle;
			rotation_angle.fromRotationMatrix(new_rotation_mat);
			if (rotation_angle.angle() < min_angle_difference && new_translation_vec.norm() < min_translation)
				return prev_error;
			
			_rotation_mat = new_rotation_mat * _rotation_mat;
			_translation_vec = new_rotation_mat * _translation_vec + new_translation_vec;

			_X = ((new_rotation_mat * _X).colwise() + new_translation_vec);
		}

		return prev_error;
	}

}