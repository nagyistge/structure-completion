#include "MeshViewerWidget.h"
#include "MeshCuboidNonLinearSolver.h"
#include "MeshCuboidPredictor.h"
#include "MeshCuboidRelation.h"
#include "MeshCuboidTrainer.h"
#include "MeshCuboidSolver.h"
#include "QGLOcculsionTestWidget.h"

#include <Eigen/Core>
#include <gflags/gflags.h>

#include <sstream>


DEFINE_string(mesh_dir, "D:/data/shape2pose/data/1_input/coseg_chairs/off/", "");
DEFINE_string(sample_dir, "D:/data/shape2pose/data/2_analysis/coseg_chairs/points/even1000/", "");
DEFINE_string(mesh_label_dir, "D:/data/shape2pose/data/1_input/coseg_chairs/gt/", "");
//DEFINE_string(mesh_label_dir, "D:/data/coseg/chairs_400/gt/four_legs/", "");
//DEFINE_string(sample_label_dir, "D:/data/shape2pose/data/4_experiments/exp3_coseg_four_legs/1_prediction/", "");
DEFINE_string(sample_label_dir, "D:/data/shape2pose/data/4_experiments/exp1_coseg_two_types/1_prediction/", "");
DEFINE_string(snapshot_dir, "output", "");

DEFINE_string(point_cuboid_label_map_filename, "point_cuboid_label_map.txt", "");
DEFINE_string(pose_filename, "pose.txt", "");
DEFINE_string(occlusion_pose_filename, "occlusion_pose.txt", "");

DEFINE_string(single_feature_filename_prefix, "single_feature_", "");
DEFINE_string(pair_feature_filename_prefix, "pair_feature_", "");
DEFINE_string(single_stats_filename_prefix, "single_stats_", "");
DEFINE_string(pair_stats_filename_prefix, "pair_stats_", "");
DEFINE_string(feature_filename_prefix, "feature_", "");
DEFINE_string(transformation_filename_prefix, "transformation_", "");
DEFINE_string(relation_filename_prefix, "joint_normal_", "");
DEFINE_string(object_list_filename, "object_list.txt", "");
DEFINE_string(prediction_stats_filename, "prediction_stat_file.csv", "");

DEFINE_double(occlusion_test_radius, 0.01, "");

const bool run_ground_truth = true;


// ---- //
// point_label_"index" -> cuboid_label.
std::vector<MeshCuboidStructure::PointCuboidLabelMap> point_cuboid_label_maps;
std::vector<Label> all_cuboid_labels;
// ---- //


void MeshViewerWidget::set_view_direction()
{
	Eigen::Vector3d view_direction;
	// -Z direction.
	view_direction << -modelview_matrix()[2], -modelview_matrix()[6], -modelview_matrix()[10];
	Eigen::Matrix3d rotation_mat;
	rotation_mat << modelview_matrix()[0], modelview_matrix()[4], modelview_matrix()[8],
		modelview_matrix()[1], modelview_matrix()[5], modelview_matrix()[9],
		modelview_matrix()[2], modelview_matrix()[6], modelview_matrix()[10];
	Eigen::Vector3d translation_vec;
	translation_vec << modelview_matrix()[12], modelview_matrix()[13], modelview_matrix()[14];
	Eigen::Vector3d view_point = -rotation_mat.transpose() * translation_vec;

	for (unsigned int i = 0; i < 3; ++i)
	{
		view_point_[i] = view_point[i];
		view_direction_[i] = view_direction[i];
	}

	update_cuboid_surface_points(cuboid_structure_, modelview_matrix());
	updateGL();
}

void MeshViewerWidget::run_occlusion_test()
{
	assert(occlusion_test_widget_);

	std::vector<Eigen::Vector3f> sample_points;
	sample_points.reserve(cuboid_structure_.num_sample_points());
	for (unsigned point_index = 0; point_index < cuboid_structure_.num_sample_points();
		++point_index)
	{
		MyMesh::Point point = cuboid_structure_.sample_points_[point_index]->point_;
		Eigen::Vector3f point_vec;
		point_vec << point[0], point[1], point[2];
		sample_points.push_back(point_vec);
	}

	occlusion_test_widget_->set_sample_points(sample_points);
	occlusion_test_widget_->set_modelview_matrix(modelview_matrix_);

	std::array<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>, 4> occlusion_test_result;
	occlusion_test_widget_->get_occlusion_test_result(
		static_cast<float>(FLAGS_occlusion_test_radius), occlusion_test_result);

	occlusion_test_points_.clear();
	occlusion_test_points_.reserve(occlusion_test_result[0].rows() * occlusion_test_result[0].cols());

	for (unsigned int row = 0; row < occlusion_test_result[0].rows(); ++row)
	{
		for (unsigned int col = 0; col < occlusion_test_result[0].cols(); ++col)
		{
			Mesh::Point point;
			for (unsigned int i = 0; i < 3; ++i)
				point[i] = static_cast<Real>(occlusion_test_result[i](row, col));
			Real value = static_cast<Real>(occlusion_test_result[3](row, col));
			assert(value >= 0);
			assert(value <= 1);

			// NOTE:
			// Reverse values.
			// After reversing, the value '1' means that the voxel is perfectly occluded.
			occlusion_test_points_.push_back(std::make_pair(point, 1.0 - value));
		}
	}

	draw_occlusion_test_points_ = true;
	//updateGL();
}

ANNkd_tree* create_occlusion_test_points_kd_tree(
	const std::vector< std::pair<MyMesh::Point, Real> > &_occlusion_test_points,
	const MeshCuboidStructure &_cuboid_structure,
	Eigen::VectorXd &_values,
	ANNpointArray &_ann_points)
{
	Eigen::MatrixXd points = Eigen::MatrixXd(3, _occlusion_test_points.size());
	_values = Eigen::VectorXd(_occlusion_test_points.size());

	for (unsigned point_index = 0; point_index < _occlusion_test_points.size();
		++point_index)
	{
		MyMesh::Point point = _occlusion_test_points[point_index].first;
		Eigen::Vector3d point_vec;
		point_vec << point[0], point[1], point[2];
		points.col(point_index) = point_vec;
		_values(point_index) = _occlusion_test_points[point_index].second;
	}
	
	ANNkd_tree* kd_tree = ICP::create_kd_tree(points, _ann_points);
	return kd_tree;
}

bool load_point_cuboid_label_map(const char *_filename, bool _verbose = true)
{
	std::ifstream file(_filename);
	if (!file)
	{
		std::cerr << "Can't open file: \"" << _filename << "\"" << std::endl;
		return false;
	}

	if (_verbose)
		std::cout << "Loading " << _filename << "..." << std::endl;

	point_cuboid_label_maps.clear();
	all_cuboid_labels.clear();

	std::string buffer;
	while (!file.eof())
	{
		std::getline(file, buffer);

		std::stringstream strstr(buffer);
		std::string token;
		if (strstr.eof()) continue;
		else std::getline(strstr, token, ',');

		//if (token.compare("num_point_labels") == 0)
		//{
		//	assert(!strstr.eof());
		//	std::getline(strstr, token, ',');
		//	num_all_point_labels = atoi(token.c_str());

		//	point_cuboid_label_map.resize(num_all_point_labels);
		//	for (std::vector<MeshCuboidStructure::PointCuboidLabelMap>::iterator it = point_cuboid_label_map.begin();
		//		it != point_cuboid_label_map.end(); ++it)
		//	{
		//		(*it).is_multiple_cuboids_ = true;
		//		(*it).mapped_cuboid_labels_.clear();
		//	}
		//}
		//else if (token.compare("num_cuboid_labels") == 0)
		//{
		//	assert(!strstr.eof());
		//	std::getline(strstr, token, ',');
		//	num_all_cuboid_labels = atoi(token.c_str());

		//	all_cuboid_labels.reserve(num_all_cuboid_labels);
		//}
		//else
		if (token.compare("p") == 0)
		{
			MeshCuboidStructure::PointCuboidLabelMap map;

			assert(!strstr.eof());
			std::getline(strstr, token, ',');
			map.is_multiple_cuboids_ = static_cast<bool>(atoi(token.c_str()));
			map.mapped_cuboid_labels_.clear();
			while (!strstr.eof())
			{
				std::getline(strstr, token, ',');
				if (token != "")
				{
					Label cuboid_label = atoi(token.c_str());
					map.mapped_cuboid_labels_.push_back(cuboid_label);
				}
			}

			point_cuboid_label_maps.push_back(map);
		}
		else if (token.compare("c") == 0)
		{
			assert(!strstr.eof());
			std::getline(strstr, token, ',');
			Label cuboid_label = atoi(token.c_str());

			all_cuboid_labels.push_back(cuboid_label);
		}
	}

	file.close();

	for (LabelIndex point_label_index = 0; point_label_index < point_cuboid_label_maps.size(); ++point_label_index)
	{
		if (point_cuboid_label_maps[point_label_index].mapped_cuboid_labels_.empty())
		{
			std::cerr << "Error: This point label index is not defined in the file (point_label_index = "
				<< point_label_index << ")." << std::endl;
			return false;
		}

		for (std::list<Label>::iterator label_it = point_cuboid_label_maps[point_label_index].mapped_cuboid_labels_.begin();
			label_it != point_cuboid_label_maps[point_label_index].mapped_cuboid_labels_.end(); ++label_it)
		{
			Label cuboid_label = (*label_it);
			if (std::find(all_cuboid_labels.begin(), all_cuboid_labels.end(), cuboid_label) == all_cuboid_labels.end())
			{
				std::cerr << "Error: This cuboid label is not defined in the file (cuboid_label = "
					<< cuboid_label << ")." << std::endl;
				return false;
			}
		}
	}

	return true;
}

void MeshViewerWidget::run_training()
{
	std::cout << "mesh_dir = " << FLAGS_mesh_dir << std::endl;
	std::cout << "sample_dir = " << FLAGS_sample_dir << std::endl;
	std::cout << "sample_label_dir = " << FLAGS_sample_label_dir << std::endl;
	std::cout << "mesh_label_dir = " << FLAGS_mesh_label_dir << std::endl;

	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	std::ofstream mesh_name_list_file(FLAGS_object_list_filename);
	assert(mesh_name_list_file);

	std::vector< std::list<MeshCuboidFeatures *> > feature_list(num_all_cuboid_labels);
	std::vector< std::list<MeshCuboidTransformation *> > transformation_list(num_all_cuboid_labels);
	//std::vector< std::list<MeshCuboidAttributes *> > attributes_list(num_all_cuboid_labels);
	//std::vector< std::list<MeshCuboidManualFeatures *> > manual_single_feature_list(num_all_cuboid_labels);
	//std::vector< std::vector< std::list<MeshCuboidManualFeatures *> > > manual_pair_feature_list(num_all_cuboid_labels);

	//for (LabelIndex all_label_index = 0; all_label_index < num_all_cuboid_labels; ++all_label_index)
	//	manual_pair_feature_list[all_label_index].resize(num_all_cuboid_labels);
	

	slotDrawMode(findAction(CUSTOM_VIEW));
	open_modelview_matrix_file(FLAGS_pose_filename.c_str());

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int i = 0; i < dir_list.size(); i++)
	{
		QFileInfo file_info = dir_list.at(i);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());
			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name;

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;

			mesh_name_list_file << mesh_name << std::endl;

			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());

			if (run_ground_truth)
			{
				//cuboid_structure_.make_mesh_vertices_as_sample_points();
				open_face_label_file(mesh_label_filename.c_str());
			}
			else
			{
				open_sample_point_label_file(sample_label_filename.c_str());
				cuboid_structure_.compute_label_cuboids();
				open_face_label_file_but_preserve_cuboids(mesh_label_filename.c_str());

				// Find the largest part for each part.
				cuboid_structure_.find_the_largest_label_cuboids();
			}

			open_modelview_matrix_file(FLAGS_pose_filename.c_str());

			mesh_.clear_colors();
			updateGL();
			slotSnapshot(snapshot_filename.c_str());


			for (LabelIndex all_label_index_1 = 0; all_label_index_1 < num_all_cuboid_labels; ++all_label_index_1)
			{
				MeshCuboidTransformation *transformation = new MeshCuboidTransformation(mesh_name);
				//MeshCuboidAttributes *attributes = new MeshCuboidAttributes(mesh_name);
				MeshCuboidFeatures *features = new MeshCuboidFeatures(mesh_name);
				//MeshSingleCuboidManualFeatures *manual_single_features = new MeshSingleCuboidManualFeatures(mesh_name);

				Label label_1 = all_cuboid_labels[all_label_index_1];
				MeshCuboid *cuboid_1 = NULL;

				// NOTE:
				// For the label, the label index in all label set
				// and the one in the proxy structure can be different.
				LabelIndex cuboid_structure_label_index_1;
				if (cuboid_structure_.exist_label(label_1, &cuboid_structure_label_index_1))
				{
					// NOTE:
					// The current implementation assumes that there is only one part for each label.
					assert(cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].size() <= 1);
					if (!cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].empty())
						cuboid_1 = cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].front();
				}

				//for (LabelIndex all_label_index_2 = all_label_index_1; all_label_index_2 < num_all_cuboid_labels; ++all_label_index_2)
				//{
				//	MeshPairCuboidManualFeatures *manual_pair_features = new MeshPairCuboidManualFeatures(mesh_name);

				//	Label label_2 = all_cuboid_labels[all_label_index_2];
				//	MeshCuboid *cuboid_2 = NULL;

				//	// NOTE:
				//	// For the label, the label index in all label set
				//	// and the one in the proxy structure can be different.
				//	LabelIndex cuboid_structure_label_index_2;
				//	if (cuboid_structure_.exist_label(label_2, &cuboid_structure_label_index_2))
				//	{
				//		// NOTE:
				//		// The current implementation assumes that there is only one part for each label.
				//		assert(cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].size() <= 1);
				//		if (!cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].empty())
				//			cuboid_2 = cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].front();
				//	}

				//	if (cuboid_1 && cuboid_2)
				//	{
				//		assert(all_label_index_1 <= all_label_index_2);
				//		manual_pair_features->compute_values(cuboid_1, cuboid_2);
				//		manual_pair_feature_list[all_label_index_1][all_label_index_2].push_back(manual_pair_features);
				//	}
				//}
				
				if (cuboid_1)
				{
					transformation->compute_transformation(cuboid_1);
					features->compute_features(cuboid_1);
					//manual_single_features->compute_values(cuboid_1);
				}

				transformation_list[all_label_index_1].push_back(transformation);
				feature_list[all_label_index_1].push_back(features);
				//manual_single_feature_list[all_label_index_1].push_back(manual_single_features);
			}
		}
	}


	for (LabelIndex all_label_index_1 = 0; all_label_index_1 < num_all_cuboid_labels; ++all_label_index_1)
	{
		//for (LabelIndex all_label_index_2 = all_label_index_1; all_label_index_2 < num_all_cuboid_labels; ++all_label_index_2)
		//{
		//	std::string pair_features_filename = FLAGS_pair_feature_filename_prefix
		//		+ std::to_string(all_label_index_1) + std::string("_")
		//		+ std::to_string(all_label_index_2) + std::string(".csv");
		//	MeshCuboidManualFeatures::save_keys_and_values(
		//		manual_pair_feature_list[all_label_index_1][all_label_index_2],
		//		pair_features_filename.c_str());

		//	std::string pair_stats_filename = FLAGS_pair_stats_filename_prefix
		//		+ std::to_string(all_label_index_1) + std::string("_")
		//		+ std::to_string(all_label_index_2) + std::string(".csv");
		//	MeshCuboidManualFeatures::save_stats(
		//		manual_pair_feature_list[all_label_index_1][all_label_index_2],
		//		pair_stats_filename.c_str());
		//}

		std::string transformation_filename = FLAGS_transformation_filename_prefix
			+ std::to_string(all_label_index_1) + std::string(".csv");
		MeshCuboidTransformation::save_transformation_collection(transformation_filename.c_str(),
			transformation_list[all_label_index_1]);

		std::string feature_filename = FLAGS_feature_filename_prefix
			+ std::to_string(all_label_index_1) + std::string(".csv");
		MeshCuboidFeatures::save_feature_collection(feature_filename.c_str(),
			feature_list[all_label_index_1]);

		//MeshCuboidAttributes::save_values(attributes_list[all_label_index_1],
		//	attributes_filename.c_str());

		//std::string single_features_filename = FLAGS_single_feature_filename_prefix
		//	+ std::to_string(all_label_index_1) + std::string(".csv");
		//MeshCuboidManualFeatures::save_keys_and_values(manual_single_feature_list[all_label_index_1],
		//	single_features_filename.c_str());

		//std::string single_stats_filename = FLAGS_single_stats_filename_prefix
		//	+ std::to_string(all_label_index_1) + std::string(".csv");
		//MeshCuboidManualFeatures::save_stats(manual_single_feature_list[all_label_index_1],
		//	single_stats_filename.c_str());
	}


	// Deallocate.
	for (LabelIndex all_label_index_1 = 0; all_label_index_1 < num_all_cuboid_labels; ++all_label_index_1)
	{
		//for (LabelIndex all_label_index_2 = all_label_index_1; all_label_index_2 < num_all_cuboid_labels; ++all_label_index_2)
		//{
		//	for (std::list<MeshCuboidManualFeatures *>::iterator it = manual_pair_feature_list[all_label_index_1][all_label_index_2].begin();
		//		it != manual_pair_feature_list[all_label_index_1][all_label_index_2].end(); ++it)
		//		delete (*it);
		//}

		for (std::list<MeshCuboidTransformation *>::iterator it = transformation_list[all_label_index_1].begin();
			it != transformation_list[all_label_index_1].end(); ++it)
			delete (*it);

		//for (std::list<MeshCuboidAttributes *>::iterator it = attributes_list[all_label_index_1].begin();
		//	it != attributes_list[all_label_index_1].end(); ++it)
		//	delete (*it);

		for (std::list<MeshCuboidFeatures *>::iterator it = feature_list[all_label_index_1].begin();
			it != feature_list[all_label_index_1].end(); ++it)
			delete (*it);

		//for (std::list<MeshCuboidManualFeatures *>::iterator it = manual_single_feature_list[all_label_index_1].begin();
		//	it != manual_single_feature_list[all_label_index_1].end(); ++it)
		//	delete (*it);
	}

	mesh_name_list_file.close();

	//
	run_training_from_files();
	//

	std::cout << std::endl;
	std::cout << " -- Batch Completed. -- " << std::endl;
}

void MeshViewerWidget::run_training_from_files()
{
	MeshCuboidJointNormalRelationTrainer trainer;
	trainer.load_object_list(FLAGS_object_list_filename);
	trainer.load_features(FLAGS_feature_filename_prefix);
	trainer.load_transformations(FLAGS_transformation_filename_prefix);

	std::vector< std::vector<MeshCuboidJointNormalRelations *> > joint_normal_relations;
	trainer.compute_relations(joint_normal_relations);

	unsigned int num_cuboids = joint_normal_relations.size();
	for (unsigned int cuboid_index_1 = 0; cuboid_index_1 < num_cuboids; ++cuboid_index_1)
	{
		assert(joint_normal_relations[cuboid_index_1].size() == num_cuboids);
		for (unsigned int cuboid_index_2 = 0; cuboid_index_2 < num_cuboids; ++cuboid_index_2)
		{
			if (cuboid_index_1 == cuboid_index_2) continue;

			const MeshCuboidJointNormalRelations *relation_12 = joint_normal_relations[cuboid_index_1][cuboid_index_2];
			if (!relation_12) continue;
			
			std::stringstream sstr;
			sstr << FLAGS_relation_filename_prefix << std::to_string(cuboid_index_1)
				<< "_" << std::to_string(cuboid_index_2) << ".csv";
			std::string relation_filename = sstr.str();

			std::cout << "Saving '" << relation_filename << "'..." << std::endl;
			relation_12->save_joint_normal_csv(relation_filename.c_str());
		}
	}
}

void MeshViewerWidget::run_prediction()
{
	std::cout << "mesh_dir = " << FLAGS_mesh_dir << std::endl;
	std::cout << "sample_dir = " << FLAGS_sample_dir << std::endl;
	std::cout << "sample_label_dir = " << FLAGS_sample_label_dir << std::endl;
	std::cout << "mesh_label_dir = " << FLAGS_mesh_label_dir << std::endl;

	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	std::ofstream prediction_stats_file(FLAGS_prediction_stats_filename);
	if (!prediction_stats_file)
	{
		std::cerr << "Can't open file: \"" << FLAGS_prediction_stats_filename << "\"" << std::endl;
		return;
	}


	MeshCuboidJointNormalRelationTrainer trainer;
	trainer.load_object_list(FLAGS_object_list_filename);
	trainer.load_features(FLAGS_feature_filename_prefix);
	trainer.load_transformations(FLAGS_transformation_filename_prefix);

	//std::vector< std::vector<MeshCuboidJointNormalRelations *> > joint_normal_relations(num_all_cuboid_labels);
	//for (LabelIndex label_index_1 = 0; label_index_1 < num_all_cuboid_labels; ++label_index_1)
	//{
	//	joint_normal_relations[label_index_1].resize(num_all_cuboid_labels, NULL);
	//	for (LabelIndex label_index_2 = 0; label_index_2 < num_all_cuboid_labels; ++label_index_2)
	//	{
	//		if (label_index_1 == label_index_2)
	//			continue;

	//		std::string relation_filename = "joint_normal_"
	//			+ std::to_string(label_index_1) + std::string("_")
	//			+ std::to_string(label_index_2) + std::string(".dat");

	//		QFileInfo relation_file(relation_filename.c_str());
	//		if (!relation_file.exists()) continue;

	//		joint_normal_relations[label_index_1][label_index_2] = new MeshCuboidJointNormalRelations();
	//		bool ret = joint_normal_relations[label_index_1][label_index_2]->load_joint_normal_dat(
	//			relation_filename.c_str());

	//		if (!ret)
	//		{
	//			do {
	//				std::cout << '\n' << "Press the Enter key to continue.";
	//			} while (std::cin.get() != '\n');
	//		}
	//	}
	//}

	//std::vector< std::vector<MeshCuboidCondNormalRelations *> > cond_normal_relations(num_all_cuboid_labels);
	//for (LabelIndex label_index_1 = 0; label_index_1 < num_all_cuboid_labels; ++label_index_1)
	//{
	//	cond_normal_relations[label_index_1].resize(num_all_cuboid_labels, NULL);
	//	for (LabelIndex label_index_2 = 0; label_index_2 < num_all_cuboid_labels; ++label_index_2)
	//	{
	//		if (label_index_1 == label_index_2)
	//			continue;

	//		std::string relation_filename = "conditional_normal_"
	//			+ std::to_string(label_index_1) + std::string("_")
	//			+ std::to_string(label_index_2) + std::string(".dat");

	//		QFileInfo relation_file(relation_filename.c_str());
	//		if (!relation_file.exists()) continue;

	//		cond_normal_relations[label_index_1][label_index_2] = new MeshCuboidCondNormalRelations();
	//		bool ret = cond_normal_relations[label_index_1][label_index_2]->load_cond_normal_dat(
	//			relation_filename.c_str());
	//		if (!ret)
	//		{
	//			do {
	//				std::cout << '\n' << "Press the Enter key to continue.";
	//			} while (std::cin.get() != '\n');
	//		}
	//	}
	//}


	slotDrawMode(findAction(CUSTOM_VIEW));

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int file_index = 0; file_index < dir_list.size(); file_index++)
	{
		QFileInfo file_info = dir_list.at(file_index);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());

			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string log_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_log.txt");

			std::string snapshot_filename_prefix = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_");
			unsigned int snapshot_index = 0;

			std::vector<MeshCuboid *> cuboids;

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;


			//
			std::list<std::string> ignored_object_list;
			ignored_object_list.push_back(mesh_name);

			std::vector< std::vector<MeshCuboidJointNormalRelations *> > joint_normal_relations;
			trainer.compute_relations(joint_normal_relations, &ignored_object_list);
			//


			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());
			open_sample_point_label_file(sample_label_filename.c_str());


			// 1. Remove occluded points.
			open_modelview_matrix_file(FLAGS_occlusion_pose_filename.c_str());
			remove_occluded_points();
			//run_occlusion_test();
			set_view_direction();

			double occlusion_modelview_matrix[16];
			memcpy(occlusion_modelview_matrix, modelview_matrix_, 16 * sizeof(double));

			//Eigen::VectorXd occlusion_test_values;
			//ANNpointArray occlusion_test_ann_points;
			//ANNkd_tree* occlusion_test_points_kd_tree = NULL;

			//occlusion_test_points_kd_tree = create_occlusion_test_points_kd_tree(
			//	occlusion_test_points_, cuboid_structure_,
			//	occlusion_test_values, occlusion_test_ann_points);
			//assert(occlusion_test_ann_points);
			//assert(occlusion_test_points_kd_tree);

			open_modelview_matrix_file(FLAGS_pose_filename.c_str());

			draw_occlusion_test_points_ = true;
			updateGL();
			slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)).c_str());
			++snapshot_index;
			draw_occlusion_test_points_ = false;


			// 2. Cluster points and construct initial cuboids.
			cuboid_structure_.compute_label_cuboids();

			//
			//ret = cuboid_structure_.apply_point_cuboid_label_map(
			//	point_cuboid_label_maps, all_cuboid_labels);
			//assert(ret);
			cuboid_structure_.apply_test();
			//

			mesh_.clear_colors();

			update_cuboid_surface_points(cuboid_structure_, occlusion_modelview_matrix);

			draw_cuboid_axes_ = false;
			updateGL();
			slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)).c_str());
			++snapshot_index;
			draw_cuboid_axes_ = true;


			// 3. Recognize labels and axes configurations.
			MeshCuboidJointNormalRelationPredictor joint_normal_predictor(joint_normal_relations);

			recognize_labels_and_axes_configurations(
				cuboid_structure_, joint_normal_predictor, log_filename.c_str());

			prediction_stats_file << mesh_name << "," << static_cast<int>(ret) << std::endl;

			// NOTE:
			// Currently, assume that there is only one cuboid for each cuboid label.
			cuboid_structure_.find_the_largest_label_cuboids();

			draw_cuboid_axes_ = true;
			updateGL();
			slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)).c_str());
			++snapshot_index;


			// 4. Segment sample points.
			segment_sample_points(cuboid_structure_);

			for (cuboid_structure_.query_label_index_ = 0;
				cuboid_structure_.query_label_index_ <= cuboid_structure_.label_cuboids_.size();
				++cuboid_structure_.query_label_index_)
			{
				updateGL();
				slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)
					+ std::string("_") + std::to_string(cuboid_structure_.query_label_index_)).c_str());
			}
			++snapshot_index;


			// 5. Optimize cuboid attributes.
			// Collect all parts.
			const double quadprog_ratio = 1E4;
			const unsigned int max_num_iterations = 10;

			optimize_attributes( cuboid_structure_, occlusion_modelview_matrix,
				joint_normal_predictor, quadprog_ratio,
				log_filename.c_str(), max_num_iterations, this);

			updateGL();
			slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)).c_str());
			++snapshot_index;

			/*
			// 6. Add missing cuboids.
			MeshCuboidCondNormalRelationPredictor cond_normal_predictor(cond_normal_relations);
			add_missing_cuboids(cuboid_structure_, cond_normal_predictor);

			updateGL();
			slotSnapshot((snapshot_filename_prefix + std::to_string(snapshot_index)).c_str());
			++snapshot_index;

			// TEST
			std::vector<MeshCuboid *> copied_cuboids;

			for (unsigned int k = 0; k < 9; ++k)
			{
				double quadprog_ratio = 1.0 - 0.1 * k;

				// Copy cuboids.
				for (std::vector<MeshCuboid *>::iterator it = copied_cuboids.begin(); it != copied_cuboids.end(); ++it)
					delete (*it);
				copied_cuboids.clear();

				copied_cuboids.reserve(cuboids.size());
				for (std::vector<MeshCuboid *>::iterator it = cuboids.begin(); it != cuboids.end(); ++it)
					copied_cuboids.push_back(new MeshCuboid(**it));


				reconstruction_snapshot_filename = snapshot_dir + std::string("/") + mesh_name + std::string("_test_") + std::to_string(k);

				reconstruct_attributes(cuboid_structure_.labels_, copied_cuboids, predictor, reconstruction_log_filename.c_str(), quadprog_ratio);

				// Put reconstructed cuboids.
				for (LabelIndex label_index = 0; label_index < cuboid_structure_.labels_.size(); ++label_index)
				{
					cuboid_structure_.label_cuboids_[label_index].clear();
					for (std::vector<MeshCuboid *>::iterator it = copied_cuboids.begin(); it != copied_cuboids.end(); ++it)
					{
						if ((*it)->get_label_index() == label_index)
							cuboid_structure_.label_cuboids_[label_index].push_back(*it);
					}
				}

				updateGL();
				slotSnapshot(reconstruction_snapshot_filename.c_str());
			}

			for (std::vector<MeshCuboid *>::iterator it = cuboids.begin(); it != cuboids.end(); ++it)
				delete (*it);
			//
			*/

			//annDeallocPts(occlusion_test_ann_points);
			//delete occlusion_test_points_kd_tree;

			for (LabelIndex label_index_1 = 0; label_index_1 < joint_normal_relations.size(); ++label_index_1)
				for (LabelIndex label_index_2 = 0; label_index_2 < joint_normal_relations[label_index_1].size(); ++label_index_2)
					delete joint_normal_relations[label_index_1][label_index_2];

			//for (LabelIndex label_index_1 = 0; label_index_1 < cond_normal_relations.size(); ++label_index_1)
			//	for (LabelIndex label_index_2 = 0; label_index_2 < cond_normal_relations[label_index_1].size(); ++label_index_2)
			//		delete cond_normal_relations[label_index_1][label_index_2];
		}
	}

	prediction_stats_file.close();
	std::cout << " -- Batch Completed. -- " << std::endl;
}

void MeshViewerWidget::run_rendering_point_clusters()
{
	std::cout << "mesh_dir = " << FLAGS_mesh_dir << std::endl;
	std::cout << "sample_dir = " << FLAGS_sample_dir << std::endl;
	std::cout << "sample_label_dir = " << FLAGS_sample_label_dir << std::endl;
	std::cout << "mesh_label_dir = " << FLAGS_mesh_label_dir << std::endl;

	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	slotDrawMode(findAction(CUSTOM_VIEW));
	open_modelview_matrix_file(FLAGS_pose_filename.c_str());

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int i = 0; i < dir_list.size(); i++)
	{
		QFileInfo file_info = dir_list.at(i);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());

			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_in");

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;

			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());
			open_sample_point_label_file(sample_label_filename.c_str());

			cuboid_structure_.compute_label_cuboids();
			updateGL();

			//
			//ret = cuboid_structure_.apply_point_cuboid_label_map(
			//	point_cuboid_label_maps, all_cuboid_labels);
			//assert(ret);
			cuboid_structure_.apply_test();
			//

			mesh_.clear_colors();

			draw_cuboid_axes_ = false;
			updateGL();
			slotSnapshot(snapshot_filename.c_str());
		}
	}

	draw_cuboid_axes_ = true;
}

/*
void MeshViewerWidget::run_test_joint_normal_training()
{
	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	std::vector< std::vector<MeshCuboidJointNormalRelations> > joint_normal_relations(num_all_cuboid_labels);
	for (LabelIndex label_index_1 = 0; label_index_1 < num_all_cuboid_labels; ++label_index_1)
	{
		joint_normal_relations[label_index_1].resize(num_all_cuboid_labels);
		for (LabelIndex label_index_2 = 0; label_index_2 < num_all_cuboid_labels; ++label_index_2)
		{
			if (label_index_1 == label_index_2)
				continue;

			std::string relation_filename = "joint_normal_"
				+ std::to_string(label_index_1) + std::string("_")
				+ std::to_string(label_index_2) + std::string(".dat");
			bool ret = joint_normal_relations[label_index_1][label_index_2].load_joint_normal_dat(
				relation_filename.c_str());
			if (!ret)
			{
				do {
					std::cout << '\n' << "Press the Enter key to continue.";
				} while (std::cin.get() != '\n');
			}
		}
	}


	slotDrawMode(findAction(CUSTOM_VIEW));

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int i = 0; i < dir_list.size(); i++)
	{
		QFileInfo file_info = dir_list.at(i);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());
			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name;

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;

			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());
			open_face_label_file(mesh_label_filename.c_str());
			open_modelview_matrix_file(FLAGS_pose_filename.c_str());

			mesh_.clear_colors();
			updateGL();

			// Find the best 'cuboid_j' w.r.t. 'cuboid_i'.
			for (LabelIndex all_label_index_1 = 0; all_label_index_1 < num_all_cuboid_labels; ++all_label_index_1)
			{
				Label label_1 = all_cuboid_labels[all_label_index_1];
				MeshCuboid *cuboid_1 = NULL;

				// NOTE:
				// For the label, the label index in all label set
				// and the one in the proxy structure can be different.
				LabelIndex cuboid_structure_label_index_1;
				if (cuboid_structure_.exist_label(label_1, &cuboid_structure_label_index_1))
				{
					// NOTE:
					// The current implementation assumes that there is only one part for each label.
					assert(cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].size() <= 1);
					if (!cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].empty())
						cuboid_1 = cuboid_structure_.label_cuboids_[cuboid_structure_label_index_1].front();
				}

				if (!cuboid_1) continue;

				for (LabelIndex all_label_index_2 = 0; all_label_index_2 < num_all_cuboid_labels; ++all_label_index_2)
				{
					if (all_label_index_1 == all_label_index_2)
						continue;

					Label label_2 = all_cuboid_labels[all_label_index_2];

					// NOTE:
					// For the label, the label index in all label set
					// and the one in the proxy structure can be different.
					LabelIndex cuboid_structure_label_index_2;
					if (cuboid_structure_.exist_label(label_2, &cuboid_structure_label_index_2))
					{
						// NOTE:
						// The current implementation assumes that there is only one part for each label.
						assert(cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].size() <= 1);
						if (!cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].empty())
						{
							assert(cuboid_1);
							MeshCuboid *&cuboid_2 = cuboid_structure_.label_cuboids_[cuboid_structure_label_index_2].front();

							MeshCuboid *new_cuboid_2 = test_joint_normal_training(
								cuboid_1, cuboid_2, all_label_index_1, all_label_index_2, joint_normal_relations);
							std::swap(cuboid_2, new_cuboid_2);

							updateGL();
							slotSnapshot((snapshot_filename + std::string("_") + std::to_string(label_1)
								+ std::string("_") + std::to_string(label_2)).c_str());

							std::swap(new_cuboid_2, cuboid_2);
							delete new_cuboid_2;
						}
					}
				}
			}

			for (LabelIndex all_label_index = 0; all_label_index < num_all_cuboid_labels; ++all_label_index)
			{
				Label label = all_cuboid_labels[all_label_index];
				LabelIndex cuboid_structure_label_index;
				if (cuboid_structure_.exist_label(label, &cuboid_structure_label_index))
				{
					for (std::vector<MeshCuboid *>::iterator it = cuboid_structure_.label_cuboids_[cuboid_structure_label_index].begin();
						it != cuboid_structure_.label_cuboids_[cuboid_structure_label_index].end(); ++it)
						(*it)->set_label_index(all_label_index);
				}
			}

			std::vector<MeshCuboid *> cuboids;
			for (std::vector< std::vector<MeshCuboid *> >::iterator it = cuboid_structure_.label_cuboids_.begin();
				it != cuboid_structure_.label_cuboids_.end(); ++it)
				cuboids.insert(cuboids.end(), (*it).begin(), (*it).end());

			MeshCuboidJointNormalRelationPredictor joint_normal_predictor(joint_normal_relations);
			optimize_attributes(cuboids, joint_normal_predictor, 0.1);

			updateGL();
			slotSnapshot(snapshot_filename.c_str());
		}
	}

	std::cout << " -- Batch Completed. -- " << std::endl;
}

void MeshViewerWidget::run_test_manual_relations()
{
	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	std::vector<MeshCuboidStats> single_stats(num_all_cuboid_labels);
	std::vector< std::vector<MeshCuboidStats> > pair_stats(num_all_cuboid_labels);

	for (LabelIndex all_label_index_1 = 0; all_label_index_1 < num_all_cuboid_labels; ++all_label_index_1)
	{
		std::string single_stats_filename = FLAGS_single_stats_filename_prefix
			+ std::to_string(all_label_index_1) + std::string(".csv");
		ret = single_stats[all_label_index_1].load_stats(single_stats_filename.c_str());
		assert(ret);

		pair_stats[all_label_index_1].resize(num_all_cuboid_labels);
		for (LabelIndex all_label_index_2 = all_label_index_1; all_label_index_2 < num_all_cuboid_labels; ++all_label_index_2)
		{
			std::string pair_stats_filename = FLAGS_pair_stats_filename_prefix
				+ std::to_string(all_label_index_1) + std::string("_")
				+ std::to_string(all_label_index_2) + std::string(".csv");
			ret = pair_stats[all_label_index_1][all_label_index_2].load_stats(pair_stats_filename.c_str());
			assert(ret);
		}
	}

	std::ofstream prediction_stats_file(FLAGS_prediction_stats_filename);
	if (!prediction_stats_file)
	{
		std::cerr << "Can't open file: \"" << FLAGS_prediction_stats_filename << "\"" << std::endl;
		return;
	}


	slotDrawMode(findAction(CUSTOM_VIEW));
	open_modelview_matrix_file(FLAGS_pose_filename.c_str());

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int i = 0; i < dir_list.size(); i++)
	{
		QFileInfo file_info = dir_list.at(i);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());
			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_in");

			std::string recognition_snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_out");
			std::string recognition_log_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string(".txt");

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;

			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());
			open_sample_point_label_file(sample_label_filename.c_str());
			cuboid_structure_.compute_label_cuboids();

			ret = cuboid_structure_.apply_point_cuboid_label_map(
				point_cuboid_label_maps, all_cuboid_labels);
			assert(ret);

			mesh_.clear_colors();
			updateGL();
			slotSnapshot(snapshot_filename.c_str());


			// Collect all parts.
			std::vector<MeshCuboid *> cuboids;
			for (std::vector< std::vector<MeshCuboid *> >::iterator it = cuboid_structure_.label_cuboids_.begin();
				it != cuboid_structure_.label_cuboids_.end(); ++it)
				cuboids.insert(cuboids.end(), (*it).begin(), (*it).end());


			MeshCuboidManualRelationPredictor predictor(single_stats, pair_stats);
			ret = recognize_labels_and_axes_configurations(
				all_cuboid_labels, cuboids, predictor, recognition_log_filename.c_str());

			prediction_stats_file << mesh_name << "," << static_cast<int>(ret) << std::endl;


			// Put recognized cuboids.
			cuboid_structure_.label_cuboids_.clear();
			cuboid_structure_.label_cuboids_.resize(num_all_cuboid_labels);
			for (LabelIndex all_label_index = 0; all_label_index < num_all_cuboid_labels; ++all_label_index)
			{
				cuboid_structure_.label_cuboids_[all_label_index].clear();
				for (std::vector<MeshCuboid *>::iterator it = cuboids.begin(); it != cuboids.end(); ++it)
				{
					if ((*it)->get_label_index() == all_label_index)
						cuboid_structure_.label_cuboids_[all_label_index].push_back(*it);
				}
			}

			updateGL();
			slotSnapshot(recognition_snapshot_filename.c_str());
		}
	}

	prediction_stats_file.close();
	std::cout << " -- Batch Completed. -- " << std::endl;
}

void MeshViewerWidget::run_test_cca_relations()
{
	bool ret;
	ret = load_point_cuboid_label_map(FLAGS_point_cuboid_label_map_filename.c_str());
	if (!ret)
	{
		do {
			std::cout << '\n' << "Press the Enter key to continue.";
		} while (std::cin.get() != '\n');
	}
	const unsigned int num_all_cuboid_labels = all_cuboid_labels.size();


	std::ofstream prediction_stats_file(FLAGS_prediction_stats_filename);
	if (!prediction_stats_file)
	{
		std::cerr << "Can't open file: \"" << FLAGS_prediction_stats_filename << "\"" << std::endl;
		return;
	}

	std::vector< std::vector< std::vector<MeshCuboidCCARelations> > > relations(num_all_cuboid_labels);
	for (LabelIndex label_index_1 = 0; label_index_1 < num_all_cuboid_labels; ++label_index_1)
	{
		relations[label_index_1].resize(num_all_cuboid_labels);
		// NOTE:
		// Use increasing order of label indices for cuboid pairs.
		// Check 'MeshCuboidPredictor.cpp' file.
		for (LabelIndex label_index_2 = label_index_1 + 1; label_index_2 < num_all_cuboid_labels; ++label_index_2)
		{
			relations[label_index_1][label_index_2].resize(num_all_cuboid_labels);

			for (LabelIndex label_index_3 = 0; label_index_3 < num_all_cuboid_labels; ++label_index_3)
			{
				std::string relation_filename = "cca_relation_"
					+ std::to_string(label_index_1) + std::string("_")
					+ std::to_string(label_index_2) + std::string("_")
					+ std::to_string(label_index_3) + std::string(".csv");
				bool ret = relations[label_index_1][label_index_2][label_index_3].load_cca_bases(
					relation_filename.c_str());
				if (!ret)
				{
					do {
						std::cout << '\n' << "Press the Enter key to continue.";
					} while (std::cin.get() != '\n');
				}
			}
		}
	}


	slotDrawMode(findAction(CUSTOM_VIEW));
	open_modelview_matrix_file(FLAGS_pose_filename.c_str());

	// For every file in the base path.
	QDir dir(FLAGS_mesh_dir.c_str());
	assert(dir.exists());
	dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	dir.setSorting(QDir::Name);


	QFileInfoList dir_list = dir.entryInfoList();
	for (int i = 0; i < dir_list.size(); i++)
	{
		QFileInfo file_info = dir_list.at(i);
		if (file_info.exists() &&
			(file_info.suffix().compare("obj") == 0
			|| file_info.suffix().compare("off") == 0))
		{
			std::string mesh_name = std::string(file_info.baseName().toLocal8Bit());
			std::string mesh_filename = std::string(file_info.filePath().toLocal8Bit());
			std::string sample_filename = FLAGS_sample_dir + std::string("/") + mesh_name + std::string(".pts");
			std::string sample_label_filename = FLAGS_sample_label_dir + std::string("/") + mesh_name + std::string(".arff");
			std::string mesh_label_filename = FLAGS_mesh_label_dir + std::string("/") + mesh_name + std::string(".seg");
			std::string snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_in");

			std::string recognition_snapshot_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string("_out");
			std::string recognition_log_filename = FLAGS_snapshot_dir + std::string("/") + mesh_name + std::string(".txt");

			QFileInfo mesh_file(mesh_filename.c_str());
			QFileInfo sample_file(sample_filename.c_str());
			QFileInfo sample_label_file(sample_label_filename.c_str());
			QFileInfo mesh_label_file(mesh_label_filename.c_str());

			if (!mesh_file.exists()
				|| !sample_file.exists()
				|| !sample_label_file.exists()
				|| !mesh_label_file.exists())
				continue;

			open_mesh_gui(mesh_filename.c_str());
			open_sample_point_file(sample_filename.c_str());
			open_sample_point_label_file(sample_label_filename.c_str());
			cuboid_structure_.compute_label_cuboids();

			ret = cuboid_structure_.apply_point_cuboid_label_map(
				point_cuboid_label_maps, all_cuboid_labels);
			assert(ret);

			mesh_.clear_colors();
			updateGL();
			slotSnapshot(snapshot_filename.c_str());


			// Collect all parts.
			std::vector<MeshCuboid *> cuboids;
			for (std::vector< std::vector<MeshCuboid *> >::iterator it = cuboid_structure_.label_cuboids_.begin();
				it != cuboid_structure_.label_cuboids_.end(); ++it)
				cuboids.insert(cuboids.end(), (*it).begin(), (*it).end());


			MeshCuboidCCARelationPredictor predictor(relations);
			bool ret = recognize_labels_and_axes_configurations(
				all_cuboid_labels, cuboids, predictor, recognition_log_filename.c_str());

			prediction_stats_file << mesh_name << "," << static_cast<int>(ret) << std::endl;


			// Put recognized cuboids.
			cuboid_structure_.label_cuboids_.clear();
			cuboid_structure_.label_cuboids_.resize(num_all_cuboid_labels);
			for (LabelIndex all_label_index = 0; all_label_index < num_all_cuboid_labels; ++all_label_index)
			{
				cuboid_structure_.label_cuboids_[all_label_index].clear();
				for (std::vector<MeshCuboid *>::iterator it = cuboids.begin(); it != cuboids.end(); ++it)
				{
					if ((*it)->get_label_index() == all_label_index)
						cuboid_structure_.label_cuboids_[all_label_index].push_back(*it);
				}
			}

			updateGL();
			slotSnapshot(recognition_snapshot_filename.c_str());
		}
	}

	prediction_stats_file.close();
	std::cout << " -- Batch Completed. -- " << std::endl;
}
*/
