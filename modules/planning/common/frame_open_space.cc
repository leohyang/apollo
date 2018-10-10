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

/**
 * @file frame_open_space.cc
 **/
#include "modules/planning/common/frame_open_space.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <list>
#include <string>
#include <utility>

#include "cybertron/common/log.h"
#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/math/vec2d.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/map/pnc_map/pnc_map.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleStateProvider;
using apollo::common::math::Box2d;
using apollo::common::math::Vec2d;
// using apollo::common::monitor::MonitorLogBuffer;
using apollo::prediction::PredictionObstacles;

constexpr double kMathEpsilon = 1e-8;

FrameOpenSpaceHistory::FrameOpenSpaceHistory()
    : IndexedQueue<uint32_t, FrameOpenSpace>(FLAGS_max_history_frame_num) {}

FrameOpenSpace::FrameOpenSpace(
    uint32_t sequence_num, const LocalView &local_view,
    const common::TrajectoryPoint &planning_start_point,
    const double start_time, const common::VehicleState &vehicle_state)
    : sequence_num_(sequence_num),
      local_view_(local_view),
      planning_start_point_(planning_start_point),
      start_time_(start_time),
      vehicle_state_(vehicle_state),
      monitor_logger_buffer_(common::monitor::MonitorMessageItem::PLANNING) {}

const common::TrajectoryPoint &FrameOpenSpace::PlanningStartPoint() const {
  return planning_start_point_;
}

const common::VehicleState &FrameOpenSpace::vehicle_state() const {
  return vehicle_state_;
}

Status FrameOpenSpace::Init() {
  hdmap_ = hdmap::HDMapUtil::BaseMapPtr();
  CHECK_NOTNULL(hdmap_);
  vehicle_state_ = common::VehicleStateProvider::Instance()->vehicle_state();
  const auto &point = common::util::MakePointENU(
      vehicle_state_.x(), vehicle_state_.y(), vehicle_state_.z());
  if (std::isnan(point.x()) || std::isnan(point.y())) {
    AERROR << "init point is not set";
    return Status(ErrorCode::PLANNING_ERROR, "init point is not set");
  }
  ADEBUG << "Enabled align prediction time ? : " << std::boolalpha
         << FLAGS_align_prediction_time;

  // prediction
  auto prediction = *(local_view_.prediction_obstacles);

  if (FLAGS_align_prediction_time) {
    AlignPredictionTime(vehicle_state_.timestamp(), &prediction);
    local_view_.prediction_obstacles->CopyFrom(prediction);
  }
  for (auto &ptr :
       Obstacle::CreateObstacles(*local_view_.prediction_obstacles)) {
    AddObstacle(*ptr);
  }
  // check collision
  if (FLAGS_enable_collision_detection) {
    const auto *collision_obstacle = FindCollisionObstacle();
    if (collision_obstacle) {
      std::string err_str =
          "Found collision with obstacle: " + collision_obstacle->Id();
      // apollo::common::monitor::MonitorLogBuffer buffer(&monitor_logger_);
      // buffer.ERROR(err_str);
      return Status(ErrorCode::PLANNING_ERROR, err_str);
    }
  }
  return Status::OK();
}

bool FrameOpenSpace::LoadDataOpenSpace() {
  // fill up the v and h presentation of the obstacle
  if (VPresentationObstacle() && HPresentationObstacle()) {
    AINFO << "fill up the v and h presentation of obstacle succeed";
    return true;
  } else {
    AINFO << "fail to get v and h presentation of obstacle";
    return false;
  }
}

const Obstacle *FrameOpenSpace::FindCollisionObstacle() const {
  if (obstacles_.Items().empty()) {
    return nullptr;
  }
  const auto &param =
      common::VehicleConfigHelper::Instance()->GetConfig().vehicle_param();
  Vec2d position(vehicle_state_.x(), vehicle_state_.y());
  Vec2d vec_to_center(
      (param.front_edge_to_center() - param.back_edge_to_center()) / 2.0,
      (param.left_edge_to_center() - param.right_edge_to_center()) / 2.0);
  Vec2d center(position + vec_to_center.rotate(vehicle_state_.heading()));
  Box2d adc_box(center, vehicle_state_.heading(), param.length(),
                param.width());
  const double adc_half_diagnal = adc_box.diagonal() / 2.0;
  for (const auto &obstacle : obstacles_.Items()) {
    if (obstacle->IsVirtual()) {
      continue;
    }

    double center_dist =
        adc_box.center().DistanceTo(obstacle->PerceptionBoundingBox().center());
    if (center_dist > obstacle->PerceptionBoundingBox().diagonal() / 2.0 +
                          adc_half_diagnal + FLAGS_max_collision_distance) {
      ADEBUG << "Obstacle : " << obstacle->Id() << " is too far to collide";
      continue;
    }
    double distance = obstacle->PerceptionPolygon().DistanceTo(adc_box);
    if (FLAGS_ignore_overlapped_obstacle && distance < kMathEpsilon) {
      bool all_points_in = true;
      for (const auto &point : obstacle->PerceptionPolygon().points()) {
        if (!adc_box.IsPointIn(point)) {
          all_points_in = false;
          break;
        }
      }
      if (all_points_in) {
        ADEBUG << "Skip overlapped obstacle, which is often caused by lidar "
                  "calibration error";
        continue;
      }
    }
    if (distance < FLAGS_max_collision_distance) {
      AERROR << "Found collision with obstacle " << obstacle->Id();
      return obstacle;
    }
  }
  return nullptr;
}

uint32_t FrameOpenSpace::SequenceNum() const { return sequence_num_; }

std::string FrameOpenSpace::DebugString() const {
  return "FrameOpenSpace: " + std::to_string(sequence_num_);
}

void FrameOpenSpace::RecordInputDebug(planning_internal::Debug *debug) {
  if (!debug) {
    ADEBUG << "Skip record input into debug";
    return;
  }

  auto *planning_debug_data = debug->mutable_planning_data();
  auto *adc_position = planning_debug_data->mutable_adc_position();
  adc_position->CopyFrom(*local_view_.localization_estimate);

  auto debug_chassis = planning_debug_data->mutable_chassis();
  debug_chassis->CopyFrom(*local_view_.chassis);

  if (!FLAGS_use_navigation_mode) {
    auto debug_routing = planning_debug_data->mutable_routing();
    debug_routing->CopyFrom(*local_view_.routing);
  }

  planning_debug_data->mutable_prediction_header()->CopyFrom(
      local_view_.prediction_obstacles->header());
}

void FrameOpenSpace::AlignPredictionTime(
    const double planning_start_time,
    PredictionObstacles *prediction_obstacles) {
  if (!prediction_obstacles || !prediction_obstacles->has_header() ||
      !prediction_obstacles->header().has_timestamp_sec()) {
    return;
  }
  double prediction_header_time =
      prediction_obstacles->header().timestamp_sec();
  for (auto &obstacle : *prediction_obstacles->mutable_prediction_obstacle()) {
    for (auto &trajectory : *obstacle.mutable_trajectory()) {
      for (auto &point : *trajectory.mutable_trajectory_point()) {
        point.set_relative_time(prediction_header_time + point.relative_time() -
                                planning_start_time);
      }
      if (!trajectory.trajectory_point().empty() &&
          trajectory.trajectory_point().begin()->relative_time() < 0) {
        auto it = trajectory.trajectory_point().begin();
        while (it != trajectory.trajectory_point().end() &&
               it->relative_time() < 0) {
          ++it;
        }
        trajectory.mutable_trajectory_point()->erase(
            trajectory.trajectory_point().begin(), it);
      }
    }
  }
}

Obstacle *FrameOpenSpace::Find(const std::string &id) {
  return obstacles_.Find(id);
}

void FrameOpenSpace::AddObstacle(const Obstacle &obstacle) {
  obstacles_.Add(obstacle.Id(), obstacle);
}

const std::vector<const Obstacle *> FrameOpenSpace::obstacles() const {
  return obstacles_.Items();
}

bool FrameOpenSpace::VPresentationObstacle() {
  // load info from pnc map
  if (!ROI()) {
    AINFO << "fail at ROI()";
    return false;
  }
  std::size_t perception_obstacles_num = obstacles_.Items().size();
  std::size_t parking_boundaries_num = ROI_warmstart_parking_boundary_.size();
  obstacles_num_ = perception_obstacles_num + parking_boundaries_num;
  if (perception_obstacles_num == 0) {
    AINFO << "no obstacle by given by percption";
  }
  // load obstacle list for warm start
  for (std::size_t i = 0; i < perception_obstacles_num; i++) {
    openspace_warmstart_obstacles_.Add(obstacles_.Items().at(i)->Id(),
                                       *(obstacles_.Items().at(i)));
  }

  // TODO(Jinyun) : depends on ROI()
  for (std::size_t i = 0; i < parking_boundaries_num; i++) {
    // load the points into Box2d, not implemented cause the order of points
    // given in ROI_warmstart_parking boundary is not implemented yet
  }

  // load vertice vector for distance approach
  Eigen::MatrixXd perception_obstacles_edges_num_ =
      4 * Eigen::MatrixXd::Ones(perception_obstacles_num, 1);
  Eigen::MatrixXd parking_boundaries_obstacles_edges_num(4, 1);
  // the order decided by the ROI()
  parking_boundaries_obstacles_edges_num << 1, 2, 2, 1;
  obstacles_edges_num_.resize(perception_obstacles_edges_num_.rows() +
                                  parking_boundaries_obstacles_edges_num.rows(),
                              1);
  obstacles_edges_num_ << perception_obstacles_edges_num_,
      parking_boundaries_obstacles_edges_num;

  // load vertices for perception obstacles(repeat the first vertice at the
  // last to form closed convex hull)
  for (const auto &obstacle : obstacles_.Items()) {
    Box2d obstacle_box = obstacle->PerceptionBoundingBox();
    std::vector<Vec2d> vertices_ccw = obstacle_box.GetAllCorners();
    std::vector<Vec2d> vertices_cw;
    while (!vertices_ccw.empty()) {
      vertices_cw.emplace_back(vertices_ccw.back());
      vertices_ccw.pop_back();
    }
    // As the obstacle is a closed convex set, the first vertice is repeated at
    // the end of the vector to help transform all four edges to inequality
    // constraint
    vertices_cw.push_back(vertices_cw.front());
    obstacles_vertices_vec_.emplace_back(vertices_cw);
  }
  // load vertices for parking boundary (not need to repeat the first vertice to
  // get close hull)
  // TODO(Jinyun) : depends on ROI()
  for (std::size_t i = 0; i < parking_boundaries_num; i++) {
    // directly load the ROI_distance_approach_parking_boundary_ into
    // obstacles_vertices_vec_
  }

  return true;
}

bool FrameOpenSpace::HPresentationObstacle() {
  obstacles_A_ = Eigen::MatrixXd::Zero(obstacles_edges_num_.sum(), 2);
  obstacles_b_ = Eigen::MatrixXd::Zero(obstacles_edges_num_.sum(), 1);
  // vertices using H-represetntation
  if (!ObsHRep(obstacles_num_, obstacles_edges_num_, obstacles_vertices_vec_,
               &obstacles_A_, &obstacles_b_)) {
    AINFO << "Fail to present obstacle in hyperplane";
    return false;
  }
  return true;
}

bool FrameOpenSpace::ObsHRep(
    const std::size_t &obstacles_num,
    const Eigen::MatrixXd &obstacles_edges_num,
    const std::vector<std::vector<Vec2d>> &obstacles_vertices_vec,
    Eigen::MatrixXd *A_all, Eigen::MatrixXd *b_all) {
  if (obstacles_num != obstacles_vertices_vec.size()) {
    AINFO << "obstacles_num != obstacles_vertices_vec.size()";
    return false;
  }

  A_all->resize(obstacles_edges_num.sum(), 2);
  b_all->resize(obstacles_edges_num.sum(), 1);

  int counter = 0;
  double kEpsilon = 1.0e-5;
  // start building H representation
  for (std::size_t i = 0; i < obstacles_num; ++i) {
    std::size_t current_vertice_num = obstacles_edges_num(i, 0);
    Eigen::MatrixXd A_i(current_vertice_num, 2);
    Eigen::MatrixXd b_i(current_vertice_num, 1);

    // take two subsequent vertices, and computer hyperplane
    for (std::size_t j = 0; j < current_vertice_num; ++j) {
      Vec2d v1 = obstacles_vertices_vec[i][j];
      Vec2d v2 = obstacles_vertices_vec[i][j + 1];

      Eigen::MatrixXd A_tmp(2, 1), b_tmp(1, 1), ab(2, 1);
      // find hyperplane passing through v1 and v2
      if (std::abs(v1.x() - v2.x()) < kEpsilon) {
        if (v2.y() < v1.y()) {
          A_tmp << 1, 0;
          b_tmp << v1.x();
        } else {
          A_tmp << -1, 0;
          b_tmp << -v1.x();
        }
      } else if (std::abs(v1.y() - v2.y()) < kEpsilon) {
        if (v1.x() < v2.x()) {
          A_tmp << 0, 1;
          b_tmp << v1.y();
        } else {
          A_tmp << 0, -1;
          b_tmp << -v1.y();
        }
      } else {
        Eigen::MatrixXd tmp1(2, 2);
        tmp1 << v1.x(), 1, v2.x(), 1;
        Eigen::MatrixXd tmp2(2, 1);
        tmp2 << v1.y(), v2.y();
        ab = tmp1.inverse() * tmp2;
        double a = ab(0, 0);
        double b = ab(1, 0);

        if (v1.x() < v2.x()) {
          A_tmp << -a, 1;
          b_tmp << b;
        } else {
          A_tmp << a, -1;
          b_tmp << -b;
        }
      }

      // store vertices
      A_i.block(j, 0, 1, 2) = A_tmp.transpose();
      b_i.block(j, 0, 1, 1) = b_tmp;
    }

    A_all->block(counter, 0, A_i.rows(), 2) = A_i;
    b_all->block(counter, 0, b_i.rows(), 1) = b_i;
    counter += current_vertice_num;
  }
  return true;
}

}  // namespace planning
}  // namespace apollo
