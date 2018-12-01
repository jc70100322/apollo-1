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

#include "modules/planning/open_space_planning.h"

#include <algorithm>
#include <limits>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "gtest/gtest_prod.h"

#include "modules/common/configs/proto/vehicle_config.pb.h"
#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/routing/proto/routing.pb.h"

#include "modules/common/math/quaternion.h"
#include "modules/common/time/time.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/planning/common/ego_info.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/common/trajectory/trajectory_stitcher.h"
#include "modules/planning/planner/rtk/rtk_replay_planner.h"
#include "modules/planning/reference_line/reference_line_provider.h"
#include "modules/planning/traffic_rules/traffic_decider.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::TrajectoryPoint;
using apollo::common::VehicleState;
using apollo::common::VehicleStateProvider;
using apollo::common::math::Box2d;
using apollo::common::math::Vec2d;
using apollo::common::time::Clock;
using apollo::dreamview::Chart;
using apollo::hdmap::HDMapUtil;
using apollo::planning_internal::OpenSpaceDebug;
using apollo::routing::RoutingResponse;

namespace {

bool IsVehicleStateValid(const VehicleState& vehicle_state) {
  if (std::isnan(vehicle_state.x()) || std::isnan(vehicle_state.y()) ||
      std::isnan(vehicle_state.z()) || std::isnan(vehicle_state.heading()) ||
      std::isnan(vehicle_state.kappa()) ||
      std::isnan(vehicle_state.linear_velocity()) ||
      std::isnan(vehicle_state.linear_acceleration())) {
    return false;
  }
  return true;
}

bool IsDifferentRouting(const RoutingResponse& first,
                        const RoutingResponse& second) {
  if (first.has_header() && second.has_header()) {
    if (first.header().sequence_num() != second.header().sequence_num()) {
      return true;
    }
    if (first.header().timestamp_sec() != second.header().timestamp_sec()) {
      return true;
    }
    return false;
  } else {
    return true;
  }
}
}  // namespace

OpenSpacePlanning::~OpenSpacePlanning() {
  planner_->Stop();
  frame_.reset(nullptr);
  planner_.reset(nullptr);
  FrameHistory::Instance()->Clear();
  last_routing_.Clear();
}

std::string OpenSpacePlanning::Name() const { return "open_space_planning"; }

Status OpenSpacePlanning::Init(const PlanningConfig& config) {
  config_ = config;
  if (!CheckPlanningConfig(config_)) {
    return Status(ErrorCode::PLANNING_ERROR,
                  "planning config error: " + config_.DebugString());
  }

  PlanningBase::Init(config_);

  planner_dispatcher_->Init();

  // load map
  hdmap_ = HDMapUtil::BaseMapPtr();
  CHECK(hdmap_) << "Failed to load map";

  // dispatch planner
  planner_ = planner_dispatcher_->DispatchPlanner();
  if (!planner_) {
    return Status(
        ErrorCode::PLANNING_ERROR,
        "planning is not initialized with config : " + config_.DebugString());
  }

  start_time_ = Clock::NowInSeconds();

  AINFO << "Open Space Planner Init Done";

  return planner_->Init(config_);
}

Status OpenSpacePlanning::InitFrame(const uint32_t sequence_num,
                                    const TrajectoryPoint& planning_start_point,
                                    const double start_time,
                                    const VehicleState& vehicle_state,
                                    ADCTrajectory* output_trajectory) {
  frame_.reset(new Frame(sequence_num, local_view_, planning_start_point,
                         start_time, vehicle_state, output_trajectory));
  if (frame_ == nullptr) {
    return Status(ErrorCode::PLANNING_ERROR, "Fail to init frame: nullptr.");
  }

  std::list<hdmap::RouteSegments> segments;

  auto status = frame_->InitForOpenSpace();

  if (!status.ok()) {
    AERROR << "failed to init frame:" << status.ToString();
    return status;
  }

  AINFO << "Open Space Planner Init Frame Done";

  return Status::OK();
}

void OpenSpacePlanning::RunOnce(const LocalView& local_view,
                                ADCTrajectory* const trajectory_pb) {
  local_view_ = local_view;
  const double start_timestamp = Clock::NowInSeconds();

  // localization
  ADEBUG << "Get localization:"
         << local_view_.localization_estimate->DebugString();

  // chassis
  ADEBUG << "Get chassis:" << local_view_.chassis->DebugString();

  Status status = VehicleStateProvider::Instance()->Update(
      *local_view_.localization_estimate, *local_view_.chassis);

  VehicleState vehicle_state =
      VehicleStateProvider::Instance()->vehicle_state();
  DCHECK_GE(start_timestamp, vehicle_state.timestamp());

  // estimate (x, y) at current timestamp
  // This estimate is only valid if the current time and vehicle state timestamp
  // differs only a small amount (20ms). When the different is too large, the
  // estimation is invalid.
  if (FLAGS_estimate_current_vehicle_state &&
      start_timestamp - vehicle_state.timestamp() < 0.020) {
    auto future_xy = VehicleStateProvider::Instance()->EstimateFuturePosition(
        start_timestamp - vehicle_state.timestamp());
    vehicle_state.set_x(future_xy.x());
    vehicle_state.set_y(future_xy.y());
    vehicle_state.set_timestamp(start_timestamp);
  }

  if (!IsVehicleStateValid(vehicle_state)) {
    std::string msg("Update VehicleStateProvider failed");
    AERROR << msg;
    status.Save(trajectory_pb->mutable_header()->mutable_status());
    FillPlanningPb(start_timestamp, trajectory_pb);
    return;
  }

  if (IsDifferentRouting(last_routing_, *local_view_.routing)) {
    last_routing_ = *local_view_.routing;
    // TODO(QiL): Get latest parking info from new routing
  }

  const double planning_cycle_time = FLAGS_open_space_planning_period;

  std::vector<TrajectoryPoint> stitching_trajectory;
  stitching_trajectory = TrajectoryStitcher::ComputeStitchingTrajectory(
      vehicle_state, start_timestamp, planning_cycle_time,
      last_publishable_trajectory_.get());

  const uint32_t frame_num = seq_num_++;
  status = InitFrame(frame_num, stitching_trajectory.back(), start_timestamp,
                     vehicle_state, trajectory_pb);

  trajectory_pb->mutable_latency_stats()->set_init_frame_time_ms(
      Clock::NowInSeconds() - start_timestamp);

  if (!status.ok()) {
    AERROR << status.ToString();
    if (FLAGS_publish_estop) {
      // Because the function "Control::ProduceControlCommand()" checks the
      // "estop" signal with the following line (Line 170 in control.cc):
      // estop_ = estop_ || trajectory_.estop().is_estop();
      // we should add more information to ensure the estop being triggered.
      ADCTrajectory estop_trajectory;
      EStop* estop = estop_trajectory.mutable_estop();
      estop->set_is_estop(true);
      estop->set_reason(status.error_message());
      status.Save(estop_trajectory.mutable_header()->mutable_status());
      FillPlanningPb(start_timestamp, &estop_trajectory);
      trajectory_pb->CopyFrom(estop_trajectory);
    } else {
      trajectory_pb->mutable_decision()
          ->mutable_main_decision()
          ->mutable_not_ready()
          ->set_reason(status.ToString());
      status.Save(trajectory_pb->mutable_header()->mutable_status());
      FillPlanningPb(start_timestamp, trajectory_pb);
    }

    FillPlanningPb(start_timestamp, trajectory_pb);
    frame_->mutable_trajectory()->CopyFrom(*trajectory_pb);
    const uint32_t n = frame_->SequenceNum();
    FrameHistory::Instance()->Add(n, std::move(frame_));
    return;
  }

  status = Plan(start_timestamp, stitching_trajectory, trajectory_pb);

  const auto time_diff_ms = (Clock::NowInSeconds() - start_timestamp) * 1000;
  ADEBUG << "total planning time spend: " << time_diff_ms << " ms.";

  trajectory_pb->mutable_latency_stats()->set_total_time_ms(time_diff_ms);
  ADEBUG << "Planning latency: "
         << trajectory_pb->latency_stats().DebugString();

  if (!status.ok()) {
    status.Save(trajectory_pb->mutable_header()->mutable_status());
    AERROR << "Planning failed:" << status.ToString();
    if (FLAGS_publish_estop) {
      AERROR << "Planning failed and set estop";
      // Because the function "Control::ProduceControlCommand()" checks the
      // "estop" signal with the following line (Line 170 in control.cc):
      // estop_ = estop_ || trajectory_.estop().is_estop();
      // we should add more information to ensure the estop being triggered.
      EStop* estop = trajectory_pb->mutable_estop();
      estop->set_is_estop(true);
      estop->set_reason(status.error_message());
    }
  }

  trajectory_pb->set_is_replan(stitching_trajectory.size() == 1);
  FillPlanningPb(start_timestamp, trajectory_pb);
  ADEBUG << "Planning pb:" << trajectory_pb->header().DebugString();

  frame_->mutable_trajectory()->CopyFrom(*trajectory_pb);

  const uint32_t n = frame_->SequenceNum();
  FrameHistory::Instance()->Add(n, std::move(frame_));
}

Status OpenSpacePlanning::Plan(
    const double current_time_stamp,
    const std::vector<TrajectoryPoint>& stitching_trajectory,
    ADCTrajectory* const trajectory_pb) {
  auto* ptr_debug = trajectory_pb->mutable_debug();

  if (FLAGS_enable_record_debug) {
    ptr_debug->mutable_planning_data()->mutable_init_point()->CopyFrom(
        stitching_trajectory.back());
  }

  auto status = planner_->Plan(stitching_trajectory.back(), frame_.get());

  if (status != Status::OK()) {
    return status;
  }

  if (FLAGS_enable_record_debug) {
    ptr_debug->mutable_planning_data()->mutable_init_point()->CopyFrom(
        stitching_trajectory.back());
    ADEBUG << "Open space init point added!";
    ptr_debug->mutable_planning_data()->mutable_open_space()->CopyFrom(
        frame_->open_space_debug());
    ADEBUG << "Open space debug information added!";
  }

  if (FLAGS_enable_record_debug && FLAGS_export_chart) {
    ExportOpenSpaceChart(frame_->open_space_debug(), ptr_debug);
    ADEBUG << "Open Space Planning debug from frame is : "
           << frame_->open_space_debug().ShortDebugString();
    ADEBUG << "Open Space Planning export chart with : "
           << trajectory_pb->ShortDebugString();
  }

  ADCTrajectory* trajectory_after_stitching_point =
      frame_->mutable_trajectory();

  trajectory_after_stitching_point->mutable_header()->set_timestamp_sec(
      current_time_stamp);

  // adjust the relative time
  int size_of_trajectory_after_stitching_point =
      trajectory_after_stitching_point->trajectory_point_size();
  double last_stitching_trajectory_relative_time =
      stitching_trajectory.back().relative_time();
  for (int i = 0; i < size_of_trajectory_after_stitching_point; i++) {
    trajectory_after_stitching_point->mutable_trajectory_point(i)
        ->set_relative_time(
            trajectory_after_stitching_point->mutable_trajectory_point(i)
                ->relative_time() +
            last_stitching_trajectory_relative_time);
  }

  last_publishable_trajectory_.reset(
      new PublishableTrajectory(*trajectory_after_stitching_point));

  ADEBUG << "current_time_stamp: " << std::to_string(current_time_stamp);

  if (FLAGS_enable_stitch_last_trajectory) {
    last_publishable_trajectory_->PrependTrajectoryPoints(
        stitching_trajectory.begin(), stitching_trajectory.end() - 1);
  }

  // trajectory partition and choose the current trajectory to follow
  auto trajectory_partition_status =
      TrajectoryPartition(last_publishable_trajectory_, trajectory_pb);

  if (trajectory_partition_status != Status::OK()) {
    return trajectory_partition_status;
  }

  BuildPredictedEnvironment(frame_.get()->obstacles());

  if (!IsCollisionFreeTrajectory(*trajectory_pb)) {
    return Status(ErrorCode::PLANNING_ERROR, "Collision Check failed");
  }

  return Status::OK();
}

Status OpenSpacePlanning::TrajectoryPartition(
    const std::unique_ptr<PublishableTrajectory>& last_publishable_trajectory,
    ADCTrajectory* const trajectory_pb) {
  std::vector<common::TrajectoryPoint> stitched_trajectory_to_end =
      last_publishable_trajectory->trajectory_points();

  double distance_s = 0.0;

  apollo::planning_internal::Trajectories trajectory_partition;
  std::vector<apollo::canbus::Chassis::GearPosition> gear_positions;

  apollo::common::Trajectory* current_trajectory =
      trajectory_partition.add_trajectory();

  // set initial gear position for first ADCTrajectory depending on v
  // and check potential edge cases
  const size_t initial_gear_check_horizon = 3;
  const double kepsilon = 1e-2;
  size_t horizon = stitched_trajectory_to_end.size();
  if (horizon < initial_gear_check_horizon)
    return Status(ErrorCode::PLANNING_ERROR, "Invalid trajectory length!");
  int direction_flag = 0;
  size_t i = 0;
  int j = 0;
  int init_direction = 0;
  while (i != initial_gear_check_horizon) {
    if (stitched_trajectory_to_end[j].v() > kepsilon) {
      i++;
      j++;
      direction_flag++;
      if (init_direction == 0) {
        init_direction++;
      }
    } else if (stitched_trajectory_to_end[j].v() < -kepsilon) {
      i++;
      j++;
      direction_flag--;
      if (init_direction == 0) {
        init_direction--;
      }
    } else {
      j++;
    }
  }
  if (direction_flag > 1) {
    gear_positions.push_back(canbus::Chassis::GEAR_DRIVE);
  } else if (direction_flag < -1) {
    gear_positions.push_back(canbus::Chassis::GEAR_REVERSE);
  } else {
    if (init_direction > 0) {
      ADEBUG << "initial speed oscillate too "
                "frequent around zero";
      gear_positions.push_back(canbus::Chassis::GEAR_DRIVE);
    } else if (init_direction < 0) {
      ADEBUG << "initial speed oscillate too "
                "frequent around zero";
      gear_positions.push_back(canbus::Chassis::GEAR_REVERSE);
    } else {
      return Status(
          ErrorCode::PLANNING_ERROR,
          "Invalid trajectory start! initial speeds too small to decide gear");
    }
  }
  // partition trajectory points into each trajectory
  for (size_t i = 0; i < horizon; i++) {
    // shift from GEAR_DRIVE to GEAR_REVERSE if v < 0
    // then add a new trajectory with GEAR_REVERSE
    if (stitched_trajectory_to_end[i].v() < -kepsilon &&
        gear_positions.back() == canbus::Chassis::GEAR_DRIVE) {
      current_trajectory = trajectory_partition.add_trajectory();
      gear_positions.push_back(canbus::Chassis::GEAR_REVERSE);
      distance_s = 0.0;
    }
    // shift from GEAR_REVERSE to GEAR_DRIVE if v > 0
    // then add a new trajectory with GEAR_DRIVE
    if (stitched_trajectory_to_end[i].v() > kepsilon &&
        gear_positions.back() == canbus::Chassis::GEAR_REVERSE) {
      current_trajectory = trajectory_partition.add_trajectory();
      gear_positions.push_back(canbus::Chassis::GEAR_DRIVE);
      distance_s = 0.0;
    }

    auto* point = current_trajectory->add_trajectory_point();

    point->set_relative_time(stitched_trajectory_to_end[i].relative_time());
    point->mutable_path_point()->set_x(
        stitched_trajectory_to_end[i].path_point().x());
    point->mutable_path_point()->set_y(
        stitched_trajectory_to_end[i].path_point().y());
    point->mutable_path_point()->set_theta(
        stitched_trajectory_to_end[i].path_point().theta());
    if (i > 0) {
      distance_s +=
          std::sqrt((stitched_trajectory_to_end[i].path_point().x() -
                     stitched_trajectory_to_end[i - 1].path_point().x()) *
                        (stitched_trajectory_to_end[i].path_point().x() -
                         stitched_trajectory_to_end[i - 1].path_point().x()) +
                    (stitched_trajectory_to_end[i].path_point().y() -
                     stitched_trajectory_to_end[i - 1].path_point().y()) *
                        (stitched_trajectory_to_end[i].path_point().y() -
                         stitched_trajectory_to_end[i - 1].path_point().y()));
    }
    point->mutable_path_point()->set_s(distance_s);
    int gear_drive = 1;
    if (gear_positions.back() == canbus::Chassis::GEAR_REVERSE) gear_drive = -1;

    point->set_v(stitched_trajectory_to_end[i].v() * gear_drive);
    // TODO(Jiaxuan): Verify this steering to kappa equation
    point->mutable_path_point()->set_kappa(
        std::tanh(stitched_trajectory_to_end[i].steer() * 470 * M_PI / 180.0 /
                  16) /
        2.85 * gear_drive);
    point->set_a(stitched_trajectory_to_end[i].a() * gear_drive);
  }

  // Choose the one to follow based on the closest partitioned trajectory
  size_t current_trajectory_index = 0;
  int closest_trajectory_point_index = 0;
  // Could have a big error in vehicle state in single thread mode!!! As the
  // vehicle state is only updated at the every beginning at RunOnce()
  VehicleState vehicle_state =
      VehicleStateProvider::Instance()->vehicle_state();
  double min_distance = std::numeric_limits<double>::max();
  for (size_t i = 0; i < gear_positions.size(); i++) {
    const apollo::common::Trajectory trajectory =
        trajectory_partition.trajectory(i);
    for (int j = 0; j < trajectory.trajectory_point_size(); j++) {
      const apollo::common::TrajectoryPoint trajectory_point =
          trajectory.trajectory_point(j);
      const apollo::common::PathPoint path_point =
          trajectory_point.path_point();
      double distance = (path_point.x() - vehicle_state.x()) *
                            (path_point.x() - vehicle_state.x()) +
                        (path_point.y() - vehicle_state.y()) *
                            (path_point.y() - vehicle_state.y());
      if (distance < min_distance) {
        min_distance = distance;
        current_trajectory_index = i;
        closest_trajectory_point_index = j;
      }
    }
  }

  trajectory_pb->mutable_trajectory_point()->CopyFrom(
      *(trajectory_partition.mutable_trajectory(current_trajectory_index)
            ->mutable_trajectory_point()));
  double time_shift =
      trajectory_pb->trajectory_point(closest_trajectory_point_index)
          .relative_time();
  int trajectory_size = trajectory_pb->trajectory_point_size();
  for (int i = 0; i < trajectory_size; i++) {
    apollo::common::TrajectoryPoint* trajectory_point =
        trajectory_pb->mutable_trajectory_point(i);
    trajectory_point->set_relative_time(trajectory_point->relative_time() -
                                        time_shift);
  }
  trajectory_pb->set_gear(gear_positions[current_trajectory_index]);

  return Status::OK();
}

void AddOpenSpaceTrajectory(const OpenSpaceDebug& open_space_debug,
                            Chart* chart) {
  chart->set_title("Open Space Trajectory Visualization");
  auto* options = chart->mutable_options();
  CHECK_EQ(open_space_debug.xy_boundary_size(), 4);
  options->mutable_x()->set_min(-20);
  options->mutable_x()->set_max(20);
  options->mutable_x()->set_label_string("x (meter)");
  options->mutable_y()->set_min(-10);
  options->mutable_y()->set_max(10);
  options->mutable_y()->set_label_string("y (meter)");
  int obstacle_index = 1;
  for (const auto& obstacle : open_space_debug.obstacles()) {
    auto* polygon = chart->add_polygon();
    polygon->set_label("boundary_" + std::to_string(obstacle_index));
    obstacle_index += 1;
    for (int vertice_index = 0;
         vertice_index < obstacle.vertices_x_coords_size(); vertice_index++) {
      auto* point_debug = polygon->add_point();
      point_debug->set_x(obstacle.vertices_x_coords(vertice_index));
      point_debug->set_y(obstacle.vertices_y_coords(vertice_index));
    }
    // Set chartJS's dataset properties
    auto* obstacle_properties = polygon->mutable_properties();
    (*obstacle_properties)["borderWidth"] = "2";
    (*obstacle_properties)["pointRadius"] = "0";
    (*obstacle_properties)["lineTension"] = "0";
    (*obstacle_properties)["fill"] = "false";
    (*obstacle_properties)["showLine"] = "true";
  }

  auto smoothed_trajectory = open_space_debug.smoothed_trajectory();
  auto* smoothed_line = chart->add_line();
  smoothed_line->set_label("smoothed");
  for (const auto& point : smoothed_trajectory.vehicle_motion_point()) {
    auto* point_debug = smoothed_line->add_point();
    point_debug->set_x(point.trajectory_point().path_point().x());
    point_debug->set_y(point.trajectory_point().path_point().y());
  }
  // Set chartJS's dataset properties
  auto* smoothed_properties = smoothed_line->mutable_properties();
  (*smoothed_properties)["borderWidth"] = "2";
  (*smoothed_properties)["pointRadius"] = "0";
  (*smoothed_properties)["fill"] = "false";
  (*smoothed_properties)["showLine"] = "true";

  auto warm_start_trajectory = open_space_debug.warm_start_trajectory();
  auto* warm_start_line = chart->add_line();
  warm_start_line->set_label("warm_start");
  for (const auto& point : warm_start_trajectory.vehicle_motion_point()) {
    auto* point_debug = warm_start_line->add_point();
    point_debug->set_x(point.trajectory_point().path_point().x());
    point_debug->set_y(point.trajectory_point().path_point().y());
  }
  // Set chartJS's dataset properties
  auto* warm_start_properties = warm_start_line->mutable_properties();
  (*warm_start_properties)["borderWidth"] = "2";
  (*warm_start_properties)["pointRadius"] = "0";
  (*warm_start_properties)["fill"] = "false";
  (*warm_start_properties)["showLine"] = "true";
}

void OpenSpacePlanning::ExportOpenSpaceChart(
    const planning_internal::OpenSpaceDebug& debug_info,
    planning_internal::Debug* debug_chart) {
  // Export Trajectory Visualization Chart.
  if (FLAGS_enable_record_debug) {
    AddOpenSpaceTrajectory(debug_info,
                           debug_chart->mutable_planning_data()->add_chart());
  }
}

bool OpenSpacePlanning::CheckPlanningConfig(const PlanningConfig& config) {
  // TODO(All): check config params
  return true;
}

void OpenSpacePlanning::FillPlanningPb(const double timestamp,
                                       ADCTrajectory* const trajectory_pb) {
  trajectory_pb->mutable_header()->set_timestamp_sec(timestamp);
  if (!local_view_.prediction_obstacles->has_header()) {
    trajectory_pb->mutable_header()->set_lidar_timestamp(
        local_view_.prediction_obstacles->header().lidar_timestamp());
    trajectory_pb->mutable_header()->set_camera_timestamp(
        local_view_.prediction_obstacles->header().camera_timestamp());
    trajectory_pb->mutable_header()->set_radar_timestamp(
        local_view_.prediction_obstacles->header().radar_timestamp());
  }
  trajectory_pb->mutable_routing_header()->CopyFrom(
      local_view_.routing->header());

  if (FLAGS_use_planning_fallback &&
      trajectory_pb->trajectory_point_size() == 0) {
    SetFallbackTrajectory(trajectory_pb);
  }
  const double dt = timestamp - Clock::NowInSeconds();
  ;
  for (auto& p : *trajectory_pb->mutable_trajectory_point()) {
    p.set_relative_time(p.relative_time() - dt);
  }
}

bool OpenSpacePlanning::IsCollisionFreeTrajectory(
    const ADCTrajectory& trajectory_pb) {
  const auto& vehicle_config =
      common::VehicleConfigHelper::Instance()->GetConfig();
  double ego_length = vehicle_config.vehicle_param().length();
  double ego_width = vehicle_config.vehicle_param().width();
  int point_size = trajectory_pb.trajectory_point().size();
  for (int i = 0; i < point_size; ++i) {
    const auto& trajectory_point = trajectory_pb.trajectory_point(i);
    double ego_theta = trajectory_point.path_point().theta();
    Box2d ego_box(
        {trajectory_point.path_point().x(), trajectory_point.path_point().y()},
        ego_theta, ego_length, ego_width);
    double shift_distance =
        ego_length / 2.0 - vehicle_config.vehicle_param().back_edge_to_center();
    Vec2d shift_vec{shift_distance * std::cos(ego_theta),
                    shift_distance * std::sin(ego_theta)};
    ego_box.Shift(shift_vec);
    if (predicted_bounding_rectangles_.size() != 0) {
      for (const auto& obstacle_box : predicted_bounding_rectangles_[i]) {
        if (ego_box.HasOverlap(obstacle_box)) {
          return false;
        }
      }
    }
  }
  return true;
}

void OpenSpacePlanning::BuildPredictedEnvironment(
    const std::vector<const Obstacle*>& obstacles) {
  predicted_bounding_rectangles_.clear();
  double relative_time = 0.0;
  while (relative_time < FLAGS_trajectory_time_length) {
    std::vector<Box2d> predicted_env;
    for (const Obstacle* obstacle : obstacles) {
      TrajectoryPoint point = obstacle->GetPointAtTime(relative_time);
      Box2d box = obstacle->GetBoundingBox(point);
      predicted_env.push_back(std::move(box));
    }
    predicted_bounding_rectangles_.push_back(std::move(predicted_env));
    relative_time += FLAGS_trajectory_time_resolution;
  }
}

}  // namespace planning
}  // namespace apollo
