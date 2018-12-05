/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include <limits>
#include <utility>

#include "Eigen/Dense"

#include "modules/common/adapters/proto/adapter_config.pb.h"
#include "modules/common/math/math_utils.h"
#include "modules/common/util/file.h"
#include "modules/map/proto/map_lane.pb.h"
#include "modules/prediction/common/feature_output.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/prediction/common/prediction_system_gflags.h"
#include "modules/prediction/common/prediction_util.h"
#include "modules/prediction/common/validation_checker.h"
#include "modules/prediction/container/container_manager.h"
#include "modules/prediction/container/obstacles/obstacles_container.h"
#include "modules/prediction/evaluator/vehicle/cruise_mlp_evaluator.h"

namespace apollo {
namespace prediction {

using apollo::common::adapter::AdapterConfig;
using apollo::common::util::GetProtoFromFile;

// Helper function for computing the mean value of a vector.
double ComputeMean(const std::vector<double>& nums, size_t start, size_t end) {
  int count = 0;
  double sum = 0.0;
  for (size_t i = start; i <= end && i < nums.size(); i++) {
    sum += nums[i];
    ++count;
  }
  return (count == 0) ? 0.0 : sum / count;
}

Eigen::MatrixXf VectorToMatrixXf(const std::vector<double>& nums,
    const int start_index, const int end_index) {
  CHECK_LT(start_index, end_index);
  CHECK_GE(start_index, 0);
  CHECK_LE(end_index, static_cast<int>(nums.size()));
  Eigen::MatrixXf output_matrix;
  output_matrix.resize(1, end_index - start_index);
  for (int i = start_index; i < end_index; ++i) {
    output_matrix(0, i - start_index) = static_cast<float>(nums[i]);
  }
  return output_matrix;
}

Eigen::MatrixXf VectorToMatrixXf(const std::vector<double>& nums,
    const int start_index, const int end_index,
    const int output_num_row, const int output_num_col) {
  CHECK_LT(start_index, end_index);
  CHECK_GE(start_index, 0);
  CHECK_LE(end_index, static_cast<int>(nums.size()));
  CHECK_EQ(end_index - start_index, output_num_row * output_num_col);
  Eigen::MatrixXf output_matrix;
  output_matrix.resize(output_num_row, output_num_col);
  int input_index = start_index;
  for (int i = 0; i < output_num_row; ++i) {
    for (int j = 0; j < output_num_col; ++j) {
      output_matrix(i, j) = static_cast<float>(nums[input_index]);
      ++input_index;
    }
  }
  CHECK_EQ(input_index, end_index);
  return output_matrix;
}

// Helper function for converting world coordinate of a point
// to relative coordinate with respect to the object (obstacle or ADC).
std::pair<double, double> WorldCoordToObjCoord
    (std::pair<double, double> input_world_coord,
     std::pair<double, double> obj_world_coord,
     double obj_world_angle) {
  double x_diff = input_world_coord.first - obj_world_coord.first;
  double y_diff = input_world_coord.second - obj_world_coord.second;
  double rho = std::sqrt(x_diff * x_diff + y_diff * y_diff);
  double theta = std::atan2(y_diff, x_diff) - obj_world_angle;

  return std::make_pair(std::cos(theta)*rho, std::sin(theta)*rho);
}

double WorldAngleToObjAngle(double input_world_angle,
                            double obj_world_angle) {
  return common::math::NormalizeAngle(input_world_angle - obj_world_angle);
}

CruiseMLPEvaluator::CruiseMLPEvaluator() {
  LoadModels(FLAGS_evaluator_cruise_vehicle_go_model_file,
             FLAGS_evaluator_cruise_vehicle_cutin_model_file);
}

void CruiseMLPEvaluator::Clear() {
}

void CruiseMLPEvaluator::Evaluate(Obstacle* obstacle_ptr) {
  // Sanity checks.
  Clear();
  CHECK_NOTNULL(obstacle_ptr);
  int id = obstacle_ptr->id();
  if (!obstacle_ptr->latest_feature().IsInitialized()) {
    AERROR << "Obstacle [" << id << "] has no latest feature.";
    return;
  }
  Feature* latest_feature_ptr = obstacle_ptr->mutable_latest_feature();
  CHECK_NOTNULL(latest_feature_ptr);
  if (!latest_feature_ptr->has_lane() ||
      !latest_feature_ptr->lane().has_lane_graph()) {
    ADEBUG << "Obstacle [" << id << "] has no lane graph.";
    return;
  }
  LaneGraph* lane_graph_ptr =
      latest_feature_ptr->mutable_lane()->mutable_lane_graph();
  CHECK_NOTNULL(lane_graph_ptr);
  if (lane_graph_ptr->lane_sequence_size() == 0) {
    AERROR << "Obstacle [" << id << "] has no lane sequences.";
    return;
  }

  ADEBUG << "There are " << lane_graph_ptr->lane_sequence_size()
         << " lane sequences with probabilities:";
  // For every possible lane sequence, extract features that are needed
  // to feed into our trained model.
  // Then compute the likelihood of the obstacle moving onto that laneseq.
  for (int i = 0; i < lane_graph_ptr->lane_sequence_size(); ++i) {
    LaneSequence* lane_sequence_ptr = lane_graph_ptr->mutable_lane_sequence(i);
    CHECK_NOTNULL(lane_sequence_ptr);
    std::vector<double> feature_values;
    ExtractFeatureValues(obstacle_ptr, lane_sequence_ptr, &feature_values);
    if (feature_values.size() != OBSTACLE_FEATURE_SIZE +
                                 INTERACTION_FEATURE_SIZE +
                                 SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
      lane_sequence_ptr->set_probability(0.0);
      ADEBUG << "Skip lane sequence due to incorrect feature size";
      continue;
    }

    if (!FLAGS_prediction_offline_mode) {
      Eigen::MatrixXf obs_feature_mat = VectorToMatrixXf(feature_values, 0,
          OBSTACLE_FEATURE_SIZE);
      Eigen::MatrixXf lane_feature_mat = VectorToMatrixXf(feature_values,
          OBSTACLE_FEATURE_SIZE + INTERACTION_FEATURE_SIZE,
          static_cast<int>(feature_values.size()), SINGLE_LANE_FEATURE_SIZE,
                           LANE_POINTS_SIZE);
      Eigen::MatrixXf model_output;
      if (lane_sequence_ptr->vehicle_on_lane()) {
        go_model_ptr_->Run({lane_feature_mat, obs_feature_mat}, &model_output);
      } else {
        cutin_model_ptr_->Run(
            {lane_feature_mat, obs_feature_mat}, &model_output);
      }
      double probability = model_output(0, 0);
      double finish_time = model_output(0, 1);
      lane_sequence_ptr->set_probability(probability);
      lane_sequence_ptr->set_time_to_lane_center(finish_time);
    }
  }

  // For offline training, append each proto output to the offline file.
  if (FLAGS_prediction_offline_mode) {
    FeatureOutput::Insert(*latest_feature_ptr);
    ADEBUG << "Insert cruise feature into feature output";
  }
}

void CruiseMLPEvaluator::ExtractFeatureValues
    (Obstacle* obstacle_ptr,
     LaneSequence* lane_sequence_ptr,
     std::vector<double>* feature_values) {
  // Sanity checks.
  CHECK_NOTNULL(obstacle_ptr);
  CHECK_NOTNULL(lane_sequence_ptr);
  int id = obstacle_ptr->id();

  // Extract obstacle related features.
  std::vector<double> obstacle_feature_values;
  SetObstacleFeatureValues(obstacle_ptr, &obstacle_feature_values);
  if (obstacle_feature_values.size() != OBSTACLE_FEATURE_SIZE) {
    ADEBUG << "Obstacle [" << id << "] has fewer than "
           << "expected obstacle feature_values "
           << obstacle_feature_values.size() << ".";
    return;
  }
  ADEBUG << "Obstacle feature size = " << obstacle_feature_values.size();
  feature_values->insert(feature_values->end(),
                         obstacle_feature_values.begin(),
                         obstacle_feature_values.end());

  // Extract interaction features.
  std::vector<double> interaction_feature_values;
  SetInteractionFeatureValues(obstacle_ptr, lane_sequence_ptr,
                              &interaction_feature_values);
  if (interaction_feature_values.size() != INTERACTION_FEATURE_SIZE) {
    ADEBUG << "Obstacle [" << id << "] has fewer than "
           << "expected lane feature_values"
           << interaction_feature_values.size() << ".";
    return;
  }
  ADEBUG << "Interaction feature size = " << interaction_feature_values.size();
  feature_values->insert(feature_values->end(),
                         interaction_feature_values.begin(),
                         interaction_feature_values.end());

  // Extract lane related features.
  std::vector<double> lane_feature_values;
  SetLaneFeatureValues(obstacle_ptr, lane_sequence_ptr, &lane_feature_values);
  if (lane_feature_values.size() !=
      SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
    ADEBUG << "Obstacle [" << id << "] has fewer than "
           << "expected lane feature_values" << lane_feature_values.size()
           << ".";
    return;
  }
  ADEBUG << "Lane feature size = " << lane_feature_values.size();
  feature_values->insert(feature_values->end(),
                         lane_feature_values.begin(),
                         lane_feature_values.end());

  // For offline training, write the extracted features into proto.
  if (FLAGS_prediction_offline_mode) {
    SaveOfflineFeatures(lane_sequence_ptr, *feature_values);
    ADEBUG << "Save cruise mlp features for obstacle ["
           << obstacle_ptr->id() << "] with dim ["
           << feature_values->size() << "]";
  }
}

void CruiseMLPEvaluator::SetObstacleFeatureValues(
    const Obstacle* obstacle_ptr, std::vector<double>* feature_values) {
  // Sanity checks and variable declarations.
  CHECK_NOTNULL(obstacle_ptr);
  feature_values->clear();
  feature_values->reserve(OBSTACLE_FEATURE_SIZE);
  std::vector<double> thetas;
  std::vector<double> lane_ls;
  std::vector<double> dist_lbs;
  std::vector<double> dist_rbs;
  std::vector<int> lane_types;
  std::vector<double> speeds;
  std::vector<double> timestamps;

  std::vector<int> has_history
      (FLAGS_cruise_historical_frame_length, 1.0);
  std::vector<std::pair<double, double>> pos_history
      (FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<std::pair<double, double>> vel_history
      (FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<std::pair<double, double>> acc_history
      (FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<double> vel_heading_history
      (FLAGS_cruise_historical_frame_length, 0.0);
  std::vector<double> vel_heading_changing_rate_history
      (FLAGS_cruise_historical_frame_length, 0.0);

  // Get obstacle's current position to set up the relative coord. system.
  const Feature& obs_curr_feature = obstacle_ptr->latest_feature();
  double obs_curr_heading = obs_curr_feature.velocity_heading();
  std::pair<double, double> obs_curr_pos =
      std::make_pair(obs_curr_feature.position().x(),
                     obs_curr_feature.position().y());
  double obs_feature_history_start_time =
      obstacle_ptr->timestamp() - FLAGS_prediction_trajectory_time_length;
  int count = 0;
  // int num_available_history_frames = 0;
  double prev_timestamp = obs_curr_feature.timestamp();

  // Starting from the most recent timestamp and going backward.
  ADEBUG << "Obstacle has " << obstacle_ptr->history_size()
         << " history timestamps.";
  for (std::size_t i = 0; i < obstacle_ptr->history_size(); ++i) {
    const Feature& feature = obstacle_ptr->feature(i);
    if (!feature.IsInitialized()) {
      continue;
    }
    if (feature.timestamp() < obs_feature_history_start_time) {
      break;
    }
    if (!feature.has_lane()) {
      ADEBUG << "Feature has no lane. Quit.";
    }
    // These are for the old 23 features.
    if (feature.has_lane() && feature.lane().has_lane_feature()) {
      thetas.push_back(feature.lane().lane_feature().angle_diff());
      lane_ls.push_back(feature.lane().lane_feature().lane_l());
      dist_lbs.push_back(feature.lane().lane_feature().dist_to_left_boundary());
      dist_rbs.push_back(
          feature.lane().lane_feature().dist_to_right_boundary());
      lane_types.push_back(feature.lane().lane_feature().lane_turn_type());
      timestamps.push_back(feature.timestamp());
      speeds.push_back(feature.speed());
      ++count;
    } else {
      ADEBUG << "Feature has no lane_feature!!!";
      ADEBUG << feature.lane().current_lane_feature_size();
    }

    // These are for the new features based on the relative coord. system.
    if (i >= FLAGS_cruise_historical_frame_length) {
      continue;
    }
    if (i != 0 && has_history[i-1] == 0.0) {
      has_history[i] = 0.0;
      continue;
    }
    if (feature.has_position()) {
      pos_history[i] = WorldCoordToObjCoord
          (std::make_pair(feature.position().x(), feature.position().y()),
           obs_curr_pos, obs_curr_heading);
    } else {
      has_history[i] = 0.0;
    }
    if (feature.has_velocity()) {
      auto vel_end = WorldCoordToObjCoord
          (std::make_pair(feature.velocity().x(), feature.velocity().y()),
           obs_curr_pos, obs_curr_heading);
      auto vel_begin = WorldCoordToObjCoord
          (std::make_pair(0.0, 0.0), obs_curr_pos, obs_curr_heading);
      vel_history[i] = std::make_pair(vel_end.first - vel_begin.first,
                                      vel_end.second - vel_begin.second);
    } else {
      has_history[i] = 0.0;
    }
    if (feature.has_acceleration()) {
      auto acc_end = WorldCoordToObjCoord
          (std::make_pair(feature.acceleration().x(),
                          feature.acceleration().y()),
           obs_curr_pos, obs_curr_heading);
      auto acc_begin = WorldCoordToObjCoord
          (std::make_pair(0.0, 0.0), obs_curr_pos, obs_curr_heading);
      acc_history[i] = std::make_pair(acc_end.first - acc_begin.first,
                                      acc_end.second - acc_begin.second);
    } else {
      has_history[i] = 0.0;
    }
    if (feature.has_velocity_heading()) {
      vel_heading_history[i] = WorldAngleToObjAngle
          (feature.velocity_heading(), obs_curr_heading);
      if (i != 0) {
        vel_heading_changing_rate_history[i] =
            (vel_heading_history[i-1] - vel_heading_history[i]) /
            (FLAGS_double_precision + feature.timestamp() - prev_timestamp);
        prev_timestamp = feature.timestamp();
      }
    } else {
      has_history[i] = 0.0;
    }
  }
  if (count <= 0) {
    ADEBUG << "There is no feature with lane info. Quit.";
    return;
  }

  // The following entire part is setting up the old 23 features.
  ///////////////////////////////////////////////////////////////
  int curr_size = 5;
  int hist_size = static_cast<int>(obstacle_ptr->history_size());
  double theta_mean = ComputeMean(thetas, 0, hist_size - 1);
  double theta_filtered = ComputeMean(thetas, 0, curr_size - 1);
  double lane_l_mean = ComputeMean(lane_ls, 0, hist_size - 1);
  double lane_l_filtered = ComputeMean(lane_ls, 0, curr_size - 1);
  double speed_mean = ComputeMean(speeds, 0, hist_size - 1);

  double time_diff = timestamps.front() - timestamps.back();
  double dist_lb_rate = (timestamps.size() > 1)
                            ? (dist_lbs.front() - dist_lbs.back()) / time_diff
                            : 0.0;
  double dist_rb_rate = (timestamps.size() > 1)
                            ? (dist_rbs.front() - dist_rbs.back()) / time_diff
                            : 0.0;

  double delta_t = 0.0;
  if (timestamps.size() > 1) {
    delta_t =
        (timestamps.front() - timestamps.back()) /
        static_cast<double>(timestamps.size() - 1);
  }
  double angle_curr = ComputeMean(thetas, 0, curr_size - 1);
  double angle_prev = ComputeMean(thetas, curr_size, 2 * curr_size - 1);
  double angle_diff =
      (hist_size >= 2 * curr_size) ? angle_curr - angle_prev : 0.0;

  double lane_l_curr = ComputeMean(lane_ls, 0, curr_size - 1);
  double lane_l_prev = ComputeMean(lane_ls, curr_size, 2 * curr_size - 1);
  double lane_l_diff =
      (hist_size >= 2 * curr_size) ? lane_l_curr - lane_l_prev : 0.0;

  double angle_diff_rate = 0.0;
  double lane_l_diff_rate = 0.0;
  if (delta_t > std::numeric_limits<double>::epsilon()) {
    angle_diff_rate = angle_diff / (delta_t * curr_size);
    lane_l_diff_rate = lane_l_diff / (delta_t * curr_size);
  }

  double acc = 0.0;
  double jerk = 0.0;
  if (static_cast<int>(speeds.size()) >= 3 * curr_size &&
      delta_t > std::numeric_limits<double>::epsilon()) {
    double speed_1st_recent =
        ComputeMean(speeds, 0, curr_size - 1);
    double speed_2nd_recent =
        ComputeMean(speeds, curr_size, 2 * curr_size - 1);
    double speed_3rd_recent =
        ComputeMean(speeds, 2 * curr_size, 3 * curr_size - 1);
    acc = (speed_1st_recent - speed_2nd_recent) / (curr_size * delta_t);
    jerk = (speed_1st_recent - 2 * speed_2nd_recent + speed_3rd_recent) /
          (curr_size * curr_size * delta_t * delta_t);
  }

  double dist_lb_rate_curr = 0.0;
  if (hist_size >= 2 * curr_size &&
      delta_t > std::numeric_limits<double>::epsilon()) {
    double dist_lb_curr = ComputeMean(dist_lbs, 0, curr_size - 1);
    double dist_lb_prev = ComputeMean(dist_lbs, curr_size, 2 * curr_size - 1);
    dist_lb_rate_curr = (dist_lb_curr - dist_lb_prev) / (curr_size * delta_t);
  }

  double dist_rb_rate_curr = 0.0;
  if (hist_size >= 2 * curr_size &&
      delta_t > std::numeric_limits<double>::epsilon()) {
    double dist_rb_curr = ComputeMean(dist_rbs, 0, curr_size - 1);
    double dist_rb_prev = ComputeMean(dist_rbs, curr_size, 2 * curr_size - 1);
    dist_rb_rate_curr = (dist_rb_curr - dist_rb_prev) / (curr_size * delta_t);
  }

  feature_values->push_back(theta_filtered);
  feature_values->push_back(theta_mean);
  feature_values->push_back(theta_filtered - theta_mean);
  feature_values->push_back(angle_diff);
  feature_values->push_back(angle_diff_rate);

  feature_values->push_back(lane_l_filtered);
  feature_values->push_back(lane_l_mean);
  feature_values->push_back(lane_l_filtered - lane_l_mean);
  feature_values->push_back(lane_l_diff);
  feature_values->push_back(lane_l_diff_rate);

  feature_values->push_back(speed_mean);
  feature_values->push_back(acc);
  feature_values->push_back(jerk);

  feature_values->push_back(dist_lbs.front());
  feature_values->push_back(dist_lb_rate);
  feature_values->push_back(dist_lb_rate_curr);

  feature_values->push_back(dist_rbs.front());
  feature_values->push_back(dist_rb_rate);
  feature_values->push_back(dist_rb_rate_curr);

  feature_values->push_back(lane_types.front() == 0 ? 1.0 : 0.0);
  feature_values->push_back(lane_types.front() == 1 ? 1.0 : 0.0);
  feature_values->push_back(lane_types.front() == 2 ? 1.0 : 0.0);
  feature_values->push_back(lane_types.front() == 3 ? 1.0 : 0.0);
  ///////////////////////////////////////////////////////////////

  /*
  for (std::size_t i = 0; i < FLAGS_cruise_historical_frame_length; i++) {
    // If not enough history, extrapolate in the following way:
    // assume constant velocity, zero acceleration, no heading change.
    // Set up position (extrapolate if not enough history):
    if (i != 0 && pos_history[i].first == 100.0) {
      feature_values->push_back(
          pos_history[i-1].first -
          vel_history[i-1].first * FLAGS_prediction_period);
      feature_values->push_back(
          pos_history[i-1].second -
          vel_history[i-1].second * FLAGS_prediction_period);
    } else {
      feature_values->push_back(pos_history[i].first);
      feature_values->push_back(pos_history[i].second);
    }
    // Set up velocity (append the same velocity if not enough history):
    if (i != 0 && vel_history[i].first == 100.0) {
      feature_values->push_back(vel_history[i-1].first);
      feature_values->push_back(vel_history[i-1].second);
    } else {
      feature_values->push_back(vel_history[i].first);
      feature_values->push_back(vel_history[i].second);
    }
    // Set up acceleration (set to zero if not enough history):
    feature_values->push_back(acc_history[i].first);
    feature_values->push_back(acc_history[i].second);
    // Set up vel heading (append the same heading if not enough history):
    if (i != 0 && vel_heading_history[i] == 100.0) {
      feature_values->push_back(vel_heading_history[i-1]);
    } else {
      feature_values->push_back(vel_heading_history[i]);
    }
    // Set up vel heading changeing rate (append zero if not enough hist.):
    feature_values->push_back(vel_heading_changing_rate_history[i]);
  }
  */
  for (std::size_t i = 0; i < FLAGS_cruise_historical_frame_length; i++) {
    feature_values->push_back(has_history[i]);
    feature_values->push_back(pos_history[i].first);
    feature_values->push_back(pos_history[i].second);
    feature_values->push_back(vel_history[i].first);
    feature_values->push_back(vel_history[i].second);
    feature_values->push_back(acc_history[i].first);
    feature_values->push_back(acc_history[i].second);
    feature_values->push_back(vel_heading_history[i]);
    feature_values->push_back(vel_heading_changing_rate_history[i]);
  }
}

void CruiseMLPEvaluator::SetInteractionFeatureValues(Obstacle* obstacle_ptr,
    LaneSequence* lane_sequence_ptr, std::vector<double>* feature_values) {
  // forward / backward: relative_s, relative_l, speed, length
  feature_values->clear();
  // Initialize forward and backward obstacles
  NearbyObstacle forward_obstacle;
  NearbyObstacle backward_obstacle;
  forward_obstacle.set_s(FLAGS_default_s_if_no_obstacle_in_lane_sequence);
  forward_obstacle.set_l(FLAGS_default_l_if_no_obstacle_in_lane_sequence);
  backward_obstacle.set_s(-FLAGS_default_s_if_no_obstacle_in_lane_sequence);
  backward_obstacle.set_l(FLAGS_default_l_if_no_obstacle_in_lane_sequence);

  for (const auto& nearby_obstacle : lane_sequence_ptr->nearby_obstacle()) {
    if (nearby_obstacle.s() < 0.0) {
      if (nearby_obstacle.s() > backward_obstacle.s()) {
        backward_obstacle.set_id(nearby_obstacle.id());
        backward_obstacle.set_s(nearby_obstacle.s());
        backward_obstacle.set_l(nearby_obstacle.l());
      }
    } else {
      if (nearby_obstacle.s() < forward_obstacle.s()) {
        forward_obstacle.set_id(nearby_obstacle.id());
        forward_obstacle.set_s(nearby_obstacle.s());
        forward_obstacle.set_l(nearby_obstacle.l());
      }
    }
  }

  auto obstacles_container = ContainerManager::Instance()->GetContainer<
      ObstaclesContainer>(AdapterConfig::PERCEPTION_OBSTACLES);
  // Set feature values for forward obstacle
  feature_values->push_back(forward_obstacle.s());
  feature_values->push_back(forward_obstacle.l());
  if (!forward_obstacle.has_id()) {  // no forward obstacle
    feature_values->push_back(0.0);
    feature_values->push_back(0.0);
  } else {
    Obstacle* forward_obs_ptr =
        obstacles_container->GetObstacle(forward_obstacle.id());
    const Feature& feature = forward_obs_ptr->latest_feature();
    feature_values->push_back(feature.length());
    feature_values->push_back(feature.speed());
  }

  // Set feature values for backward obstacle
  feature_values->push_back(backward_obstacle.s());
  feature_values->push_back(backward_obstacle.l());
  if (!backward_obstacle.has_id()) {  // no forward obstacle
    feature_values->push_back(0.0);
    feature_values->push_back(0.0);
  } else {
    Obstacle* backward_obs_ptr =
        obstacles_container->GetObstacle(backward_obstacle.id());
    const Feature& feature = backward_obs_ptr->latest_feature();
    feature_values->push_back(feature.length());
    feature_values->push_back(feature.speed());
  }
}

void CruiseMLPEvaluator::SetLaneFeatureValues
    (const Obstacle* obstacle_ptr, const LaneSequence* lane_sequence_ptr,
     std::vector<double>* feature_values) {
  // Sanity checks.
  feature_values->clear();
  feature_values->reserve(SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE);
  const Feature& feature = obstacle_ptr->latest_feature();
  if (!feature.IsInitialized()) {
    ADEBUG << "Obstacle [" << obstacle_ptr->id() << "] has no latest feature.";
    return;
  } else if (!feature.has_position()) {
    ADEBUG << "Obstacle [" << obstacle_ptr->id() << "] has no position.";
    return;
  }

  double heading = feature.velocity_heading();
  // double speed = feature.speed();
  for (int i = 0; i < lane_sequence_ptr->lane_segment_size(); ++i) {
    if (feature_values->size() >= SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
      break;
    }
    const LaneSegment& lane_segment = lane_sequence_ptr->lane_segment(i);
    for (int j = 0; j < lane_segment.lane_point_size(); ++j) {
      if (feature_values->size() >=
          SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
        break;
      }
      const LanePoint& lane_point = lane_segment.lane_point(j);
      if (!lane_point.has_position()) {
        AERROR << "Lane point has no position.";
        continue;
      }

      std::pair<double, double> relative_s_l = WorldCoordToObjCoord
      (std::make_pair(lane_point.position().x(), lane_point.position().y()),
       std::make_pair(feature.position().x(), feature.position().y()),
       heading);
      double relative_ang = WorldAngleToObjAngle(lane_point.heading(),
                                                 heading);

      feature_values->push_back(relative_s_l.second);
      feature_values->push_back(relative_s_l.first);
      feature_values->push_back(relative_ang);
      // feature_values->push_back(speed * speed * lane_point.kappa());
      feature_values->push_back(lane_point.kappa());
    }
  }

  // If the lane points are not sufficient, apply a linear extrapolation.
  std::size_t size = feature_values->size();
  while (size >= 10 && size < SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
    double relative_l_new = 2 * feature_values->operator[](size - 5) -
                                feature_values->operator[](size - 10);
    double relative_s_new = 2 * feature_values->operator[](size - 4) -
                                feature_values->operator[](size - 9);
    double relative_ang_new = feature_values->operator[](size - 3);
    // double centri_acc_new = 0.0;

    feature_values->push_back(relative_l_new);
    feature_values->push_back(relative_s_new);
    feature_values->push_back(relative_ang_new);
    // feature_values->push_back(centri_acc_new);
    feature_values->push_back(0.0);

    size = feature_values->size();
  }
}

void CruiseMLPEvaluator::LoadModels(const std::string& go_model_file,
    const std::string& cutin_model_file) {
  go_model_ptr_.reset(new network::CruiseModel());
  cutin_model_ptr_.reset(new network::CruiseModel());
  CHECK_NOTNULL(go_model_ptr_);
  CHECK_NOTNULL(cutin_model_ptr_);

  AINFO << "start loading models";

  CruiseModelParameter go_model_param;
  CruiseModelParameter cutin_model_param;
  CHECK(GetProtoFromFile(go_model_file, &go_model_param))
      << "Unable to load go model file: " << go_model_file << ".";
  CHECK(GetProtoFromFile(cutin_model_file, &cutin_model_param))
      << "Unable to load cut-in model file: " << cutin_model_file << ".";

  go_model_ptr_->LoadModel(go_model_param);
  cutin_model_ptr_->LoadModel(cutin_model_param);

  AINFO << "Succeeded in loading go model: " << go_model_file << ".";
  AINFO << "Succeeded in loading cut-in model: " << cutin_model_file << ".";
}

// TODO(all): implement this once the model is trained and ready.
double CruiseMLPEvaluator::ComputeFinishTime(
    const std::vector<double>& feature_values) {
  return 6.0;
}

void CruiseMLPEvaluator::SaveOfflineFeatures(
    LaneSequence* sequence, const std::vector<double>& feature_values) {
  for (double feature_value : feature_values) {
    sequence->mutable_features()->add_mlp_features(feature_value);
  }
}

}  // namespace prediction
}  // namespace apollo
