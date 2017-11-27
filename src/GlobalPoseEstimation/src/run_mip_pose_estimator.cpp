/*
 */

#include <string>
#include <stdexcept>
#include <iostream>
#include <random>
#include <unistd.h>
#include <typeinfo>

#include "drake/math/quaternion.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/rigid_body_tree.h"
#include "drake/multibody/parsers/urdf_parser.h"

#include "common/common.hpp"
#include "common/common_vtk.hpp"
#include "yaml-cpp/yaml.h"

#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>

#include "RemoteTreeViewerWrapper.hpp"

#include <vtkSmartPointer.h>
#include <vtkActor.h>
#include <vtkDoubleArray.h>
#include <vtkPointData.h>
#include <vtkPointSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataPointSampler.h>
#include <vtkProperty.h>
#include <vtkSmartPointer.h>
#include <vtksys/SystemTools.hxx>

#include "mip_pose_estimator.hpp"

using namespace std;
using namespace Eigen;
using namespace drake::parsers::urdf;

typedef pcl::PointXYZ PointType;

int main(int argc, char** argv) {
  srand(0);

  if (argc < 3){
    printf("Use: run_mip_pose_estimator <point cloud file, vtp> <model file, yaml> <config file> <optional output_file>\n");
    exit(-1);
  }

  time_t _tm =time(NULL );
  struct tm * curtime = localtime ( &_tm );
  cout << "***************************" << endl;
  cout << "***************************" << endl;
  cout << "MIQP Multiple Mesh Model Object Pose Estimator" << asctime(curtime);
  cout << "Point cloud file " << string(argv[1]) << endl;
  cout << "Model config file " << string(argv[2]) << endl;
  cout << "Config file " << string(argv[3]) << endl;
  if (argc > 3)
    cout << "Output file " << string(argv[4]) << endl;
  cout << "***************************" << endl << endl;

  // Bring in config file
  string sceneFile = string(argv[1]);
  string modelYamlFile = string(argv[2]);
  string yamlString = string(argv[3]);
  YAML::Node modelConfig = YAML::LoadFile(modelYamlFile);
  YAML::Node config = YAML::LoadFile(yamlString);

  if (config["detector_options"] == NULL){
    runtime_error("Need detector options.");
  }

  // Set up model
  if (modelConfig["models"] == NULL){
    runtime_error("Model must be specified.");
  }

  if (config["detector_options"] == NULL){
    runtime_error("Config needs a detector option set.");
  }

  // Model will be a RigidBodyTree.
  RigidBodyTree<double> robot;
  VectorXd q_robot;
  int old_q_robot_size = 0;
  for (auto iter=modelConfig["models"].begin(); iter!=modelConfig["models"].end(); iter++){
    string urdf = (*iter)["urdf"].as<string>();
    AddModelInstanceFromUrdfFileWithRpyJointToWorld(urdf, &robot);
    
    vector<double> q0 = (*iter)["q0"].as<vector<double>>();
    if (q0.size() == 7) {
      printf("Converting init cond from quaternion to rpy.\n");
      auto rpy = drake::math::quat2rpy(Vector4d(q0[3], q0[4], q0[5], q0[6]));
      q0[3] = rpy[0];
      q0[4] = rpy[1];
      q0[5] = rpy[2];
      q0.resize(6);
    } else if (q0.size() != 6) {
      printf("q0 had %lu positions, which doesn't make sense.\n", q0.size());
      exit(0);
    }
      
    // And add initial state info that we were passed
    assert(robot.get_num_positions() - old_q_robot_size == q0.size());
    q_robot.conservativeResize(robot.get_num_positions());
    for (int i=0; i<q0.size(); i++){
      q_robot[old_q_robot_size] = q0[i];
      old_q_robot_size++; 
    }
  }
  robot.compile();

  // Load in the scene cloud and downsample it if requested
  double downsample_spacing = -1.0;
  if (config["detector_options"]["downsample_spacing"])
    downsample_spacing = config["detector_options"]["downsample_spacing"].as<double>();
  Matrix3Xd scene_pts = LoadAndDownsamplePolyData(sceneFile, downsample_spacing);

  // Visualize the scene points and GT, to start with.
  RemoteTreeViewerWrapper rm;
  // Publish the scene cloud
  rm.publishPointCloud(scene_pts, {"mip", "scene_pts_loaded"}, {{0.1, 1.0, 0.1}});
  rm.publishRigidBodyTree(robot, q_robot, Vector4d(1.0, 0.6, 0.1, 0.5), {"mip", "robot_gt"});


  // Load in MIP estimator
  MIPMultipleMeshPoseEstimator estimator(config["detector_options"], modelConfig);
  auto solutions = estimator.doObjectPoseEstimation(scene_pts);
  VectorXd q_robot_est = q_robot * 0;

  // Iterate through the generated solutions, and:
  //   1) Visualize them on the remote tree viewer
  //   2) Save them out in json format

  // Remote Tree Viewer Vis
  for (const auto& solution : solutions){
    stringstream sol_name;
    sol_name << "sol_obj_" << solution.objective << estimator.getDetailName();
    for (const auto& detection : solution.pose_estimates){
      // Visualize the estimated body pose
      const RigidBody<double>& body = estimator.get_robot().get_body(detection.obj_ind);
      for (const auto& collision_elem_id : body.get_collision_element_ids()) {
        stringstream collision_elem_id_str;
        collision_elem_id_str << collision_elem_id;
        auto element = estimator.get_robot().FindCollisionElement(collision_elem_id);
        if (element->hasGeometry()){
          vector<string> path;
          path.push_back("mip");
          path.push_back(sol_name.str());
          path.push_back(body.get_name());
          path.push_back(collision_elem_id_str.str());
          rm.publishGeometry(element->getGeometry(), detection.est_tf * element->getLocalTransform(), Vector4d(0.2, 0.2, 1.0, 0.5), path);
        }
      }

      // Also visualize the correspondences that led us to generate this, by publishing
      // the model points and lines between model and scene points
      Matrix3Xd model_pts_world(3, detection.correspondences.size());
      int i = 0;
      for (const auto& corresp : detection.correspondences) {
        model_pts_world.col(i) = corresp.model_pt;
        i++;
      }
      model_pts_world = detection.est_tf * model_pts_world;
      rm.publishPointCloud(model_pts_world, {"mip", "correspondences", "model pts", body.get_name()}, {{0.1, 0.1, 1.0}});

      // And extract the joint coordinates (assumes all bodies are floating bases...)
      q_robot_est.block(body.get_position_start_index(), 0, 3, 1) =
        detection.est_tf.translation();
      q_robot_est.block(body.get_position_start_index()+3, 0, 3, 1) = 
        drake::math::rotmat2rpy(detection.est_tf.rotation());
    }
  }

  // Pre-process history into separate vectors
  auto history = estimator.get_solve_history();
  vector<double> wall_times;
  vector<double> reported_runtimes;
  vector<double> best_objectives;
  vector<double> best_bounds;
  vector<double> explored_node_counts;
  vector<double> feasible_solutions_counts;

  for (const auto& elem : history){
    wall_times.push_back(elem.wall_time);
    reported_runtimes.push_back(elem.reported_runtime);
    best_objectives.push_back(elem.best_objective);
    best_bounds.push_back(elem.best_bound);
    explored_node_counts.push_back(elem.explored_node_count);
    feasible_solutions_counts.push_back(elem.feasible_solutions_count);
  }

  // Solution save-out
  // TODO(gizatt) This doesn't work completely yet, as
  // the MIP mesh model estimator generates
  // pose estimates in maximal coords, while
  // we want to save out estimates in the joint
  // space of the robot. We'll need to write a 
  // projection operator somehow to make this
  // saving work, or switch the other 
  // estimators to save out to maximal coords
  // as well (which might be nice). For now,
  // I'm more concerned about general runtime
  // and functionality...
  // A possible route for doing that projection
  // would be forming a call to Drake's InverseKin
  // which will do the projection as an NLP.
  if (argc > 3){
    string outputFilename = string(argv[4]);

    YAML::Emitter out;

    out << YAML::BeginMap; {
      out << YAML::Key << "scene";
      out << YAML::Value << sceneFile;

      out << YAML::Key << "config";
      out << YAML::Value << config;

      out << YAML::Key << "solutions";
      out << YAML::BeginSeq;
      for (const auto& solution : solutions){
        out << YAML::BeginMap; {
          out << YAML::Key << "objective";
          out << YAML::Value << solution.objective;
          out << YAML::Key << "bound";
          out << YAML::Value << solution.lower_bound;
          out << YAML::Key << "solve_time";
          out << YAML::Key << solution.solve_time;

          out << YAML::Key << "models";
          out << YAML::Value << YAML::BeginSeq; {
            VectorXd q_robot_this;
            for (int i = 0; i < robot.get_num_model_instances(); i++) {
              auto bodies = robot.FindModelInstanceBodies(i);
              for (const auto& body : bodies){
                int old_size = q_robot_this.rows();
                int new_rows = body->getJoint().get_num_positions();
                q_robot_this.conservativeResize(old_size + new_rows);
                q_robot_this.block(old_size, 0, new_rows, 1) = 
                  q_robot_est.block(body->get_position_start_index(), 0, new_rows, 1);
              }

              out << YAML::BeginMap; {
                out << YAML::Key << "urdf";
                // I assume the model instances are in same order as model members in the config yaml...
                out << YAML::Key << modelConfig["models"][i]["urdf"].as<string>();
                out << YAML::Key << "q";
                out << YAML::Value << YAML::Flow << vector<double>(q_robot_this.data(), q_robot_this.data() + q_robot_this.rows());
              } out << YAML::EndMap;
            }  
          } out << YAML::EndSeq;

          out << YAML::Key << "tfs";
          out << YAML::Value << YAML::BeginSeq; {
            for (const auto& detection : solution.pose_estimates){
              out << YAML::BeginMap; {
                out << YAML::Key << "obj_ind" << YAML::Value << detection.obj_ind;
                out << YAML::Key << "R";
                MatrixXd R = detection.R_fit.transpose();
                out << YAML::Value << YAML::Flow << vector<double>(R.data(), R.data() + R.rows() * R.cols());
                out << YAML::Key << "T";
                MatrixXd T = detection.T_fit;
                out << YAML::Value << YAML::Flow << vector<double>(T.data(), T.data() + T.rows() * T.cols());
              } out << YAML::EndMap;
            }
          } out << YAML::EndSeq;

          out << YAML::Key << "history";
          out << YAML::Value << YAML::BeginMap; {
            out << YAML::Key << "wall_time";
            out << YAML::Value << YAML::Flow << wall_times;
            out << YAML::Key << "reported_runtime";
            out << YAML::Value << YAML::Flow << reported_runtimes;
            out << YAML::Key << "best_objective";
            out << YAML::Value << YAML::Flow << best_objectives;
            out << YAML::Key << "best_bound";
            out << YAML::Value << YAML::Flow << best_bounds;
            out << YAML::Key << "explored_node_count";
            out << YAML::Value << YAML::Flow << explored_node_counts;
            out << YAML::Key << "feasible_solutions_count";
            out << YAML::Value << YAML::Flow << feasible_solutions_counts;
          } out << YAML::EndMap;

        } out << YAML::EndMap;
      } out << YAML::EndSeq;
    } out << YAML::EndMap;

    ofstream fout(outputFilename);
    fout << out.c_str();
    fout.close();
  }

  return 0;
}
