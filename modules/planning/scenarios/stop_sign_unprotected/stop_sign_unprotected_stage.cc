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
 * @file
 **/

#include "modules/planning/scenarios/stop_sign_unprotected/stop_sign_unprotected_stage.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "modules/perception/proto/perception_obstacle.pb.h"

#include "cyber/common/log.h"
#include "modules/common/time/time.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/common/planning_context.h"
#include "modules/planning/toolkits/deciders/decider_creep.h"

namespace apollo {
namespace planning {
namespace scenario {
namespace stop_sign_protected {

using apollo::common::Status;
using common::ErrorCode;
using common::TrajectoryPoint;
using common::time::Clock;
using hdmap::HDMapUtil;
using hdmap::LaneInfo;
using hdmap::LaneInfoConstPtr;
using hdmap::OverlapInfoConstPtr;
using hdmap::StopSignInfo;
using hdmap::StopSignInfoConstPtr;
using perception::PerceptionObstacle;

using StopSignLaneVehicles =
    std::unordered_map<std::string, std::vector<std::string>>;

Stage::StageStatus StopSignUnprotectedPreStop::Process(
    const TrajectoryPoint& planning_init_point, Frame* frame) {
  CHECK_NOTNULL(frame);

  const auto& reference_line_info = frame->reference_line_info().front();
  if (CheckADCStop(reference_line_info)) {
    GetContext()->stop_start_time = Clock::NowInSeconds();

    next_stage_ = ScenarioConfig::STOP_SIGN_UNPROTECTED_STOP;
    return Stage::FINISHED;
  }

  const PathDecision& path_decision = reference_line_info.path_decision();
  auto& watch_vehicles = GetContext()->watch_vehicles;
  for (const auto* path_obstacle : path_decision.path_obstacles().Items()) {
    // add to watch_vehicles if adc is still proceeding to stop sign
    AddWatchVehicle(*path_obstacle, &watch_vehicles);
  }

  bool plan_ok = PlanningOnReferenceLine(planning_init_point, frame);
  if (!plan_ok) {
    AERROR << "StopSignUnprotectedPreStop planning error";
  }
  return Stage::RUNNING;
}

Stage::StageStatus StopSignUnprotectedStop::Process(
    const TrajectoryPoint& planning_init_point, Frame* frame) {
  CHECK_NOTNULL(frame);

  auto start_time = GetContext()->stop_start_time;
  double wait_time = Clock::NowInSeconds() - start_time;
  ADEBUG << "stop_start_time[" << start_time << "] wait_time[" << wait_time
         << "]";
  auto& watch_vehicles = GetContext()->watch_vehicles;
  if (wait_time >= conf_stop_duration_ && watch_vehicles.empty()) {
    next_stage_ = ScenarioConfig::STOP_SIGN_UNPROTECTED_CREEP;
    return Stage::FINISHED;
  }

  // check timeout
  if (wait_time > conf_wait_timeout_ && watch_vehicles.size() <= 1) {
    next_stage_ = ScenarioConfig::STOP_SIGN_UNPROTECTED_CREEP;
    return Stage::FINISHED;
  }

  // get all vehicles currently watched
  std::vector<std::string> watch_vehicle_ids;
  for (auto it = watch_vehicles.begin(); it != watch_vehicles.end(); ++it) {
    std::copy(it->second.begin(), it->second.end(),
              std::back_inserter(watch_vehicle_ids));
  }

  const auto& reference_line_info = frame->reference_line_info().front();
  const PathDecision& path_decision = reference_line_info.path_decision();
  for (const auto* path_obstacle : path_decision.path_obstacles().Items()) {
    // remove from watch_vehicles_ if adc is stopping/waiting at stop sign
    RemoveWatchVehicle(*path_obstacle, watch_vehicle_ids, &watch_vehicles);
  }

  return Stage::RUNNING;
}

Stage::StageStatus StopSignUnprotectedCreep::Process(
    const common::TrajectoryPoint& planning_init_point, Frame* frame) {
  CHECK_NOTNULL(frame);

  if (!config_.enabled()) {
    next_stage_ = ScenarioConfig::STOP_SIGN_UNPROTECTED_INTERSECTION_CRUISE;
    return Stage::FINISHED;
  }

  auto& reference_line_info = frame->mutable_reference_line_info()->front();
  if (dynamic_cast<DeciderCreep*>(FindTask(TaskConfig::DECIDER_CREEP))
          ->CheckCreepDone(*frame, reference_line_info,
                           GetContext()->next_stop_sign_overlap.end_s)) {
    return Stage::FINISHED;
  }

  /*
  // build a stop fence by creep decider
  if (dynamic_cast<DeciderCreep*>(FindTask(TaskConfig::DECIDER_CREEP))
          ->Process(frame, &reference_line_info) != Status::OK()) {
    ADEBUG << "fail at build stop fence at creeping";
    return Stage::ERROR;
  }
  */

  // set param for PROCEED_WITH_CAUTION_SPEED
  dynamic_cast<DeciderCreep*>(FindTask(TaskConfig::DECIDER_CREEP))
      ->SetProceedWithCautionSpeedParam(*frame, reference_line_info);

  bool plan_ok = PlanningOnReferenceLine(planning_init_point, frame);
  if (!plan_ok) {
    AERROR << "StopSignUnprotectedCreep planning error";
  }
  return Stage::RUNNING;
}

Stage::StageStatus StopSignUnprotectedIntersectionCruise::Process(
    const common::TrajectoryPoint& planning_init_point, Frame* frame) {
  CHECK_NOTNULL(frame);

  if (GetContext()->next_stop_sign_overlap.object_id !=
      PlanningContext::GetScenarioInfo()->next_stop_sign_overlap.object_id) {
    next_stage_ = ScenarioConfig::NO_STAGE;
    return Stage::FINISHED;
  }

  bool plan_ok = PlanningOnReferenceLine(planning_init_point, frame);
  if (!plan_ok) {
    AERROR << "StopSignUnprotectedIntersectionCruise plan error";
  }
  return Stage::RUNNING;
}

/*
 * get all the lanes associated/guarded by a stop sign
 */
int StopSignUnprotectedScenario::GetAssociatedLanes(
    const StopSignInfo& stop_sign_info) {
  context_.associated_lanes.clear();

  std::vector<StopSignInfoConstPtr> associated_stop_signs;
  HDMapUtil::BaseMap().GetStopSignAssociatedStopSigns(stop_sign_info.id(),
                                                      &associated_stop_signs);

  for (const auto stop_sign : associated_stop_signs) {
    if (stop_sign == nullptr) {
      continue;
    }

    const auto& associated_lane_ids = stop_sign->OverlapLaneIds();
    for (const auto& lane_id : associated_lane_ids) {
      const auto lane = HDMapUtil::BaseMap().GetLaneById(lane_id);
      if (lane == nullptr) {
        continue;
      }
      const auto& stop_sign_overlaps = lane->stop_signs();
      for (auto stop_sign_overlap : stop_sign_overlaps) {
        auto over_lap_info =
            stop_sign_overlap->GetObjectOverlapInfo(stop_sign.get()->id());
        if (over_lap_info != nullptr) {
          context_.associated_lanes.push_back(
              std::make_pair(lane, stop_sign_overlap));
          ADEBUG << "stop_sign: " << stop_sign_info.id().id()
                 << "; associated_lane: " << lane_id.id()
                 << "; associated_stop_sign: " << stop_sign.get()->id().id();
        }
      }
    }
  }

  return 0;
}

/**
 * @brief: add a watch vehicle which arrives at stop sign ahead of adc
 */
int StopSignUnprotectedPreStop::AddWatchVehicle(
    const PathObstacle& path_obstacle, StopSignLaneVehicles* watch_vehicles) {
  CHECK_NOTNULL(watch_vehicles);

  const PerceptionObstacle& perception_obstacle =
      path_obstacle.obstacle()->Perception();
  const std::string& obstacle_id = std::to_string(perception_obstacle.id());
  PerceptionObstacle::Type obstacle_type = perception_obstacle.type();
  std::string obstacle_type_name = PerceptionObstacle_Type_Name(obstacle_type);

  // check type
  if (obstacle_type != PerceptionObstacle::UNKNOWN &&
      obstacle_type != PerceptionObstacle::UNKNOWN_MOVABLE &&
      obstacle_type != PerceptionObstacle::BICYCLE &&
      obstacle_type != PerceptionObstacle::VEHICLE) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "]. skip";
    return 0;
  }

  auto point = common::util::MakePointENU(perception_obstacle.position().x(),
                                          perception_obstacle.position().y(),
                                          perception_obstacle.position().z());
  double obstacle_s = 0.0;
  double obstacle_l = 0.0;
  hdmap::LaneInfoConstPtr obstacle_lane;
  if (HDMapUtil::BaseMap().GetNearestLaneWithHeading(
          point, 5.0, perception_obstacle.theta(), M_PI / 3.0, &obstacle_lane,
          &obstacle_s, &obstacle_l) != 0) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "]: Failed to find nearest lane from map for position: "
           << point.DebugString()
           << "; heading:" << perception_obstacle.theta();
    return -1;
  }

  // check obstacle is on an associate lane guarded by stop sign
  std::string obstable_lane_id = obstacle_lane.get()->id().id();
  auto assoc_lane_it = std::find_if(
      GetContext()->associated_lanes.begin(),
      GetContext()->associated_lanes.end(),
      [&obstable_lane_id](
          std::pair<LaneInfoConstPtr, OverlapInfoConstPtr>& assc_lane) {
        return assc_lane.first.get()->id().id() == obstable_lane_id;
      });
  if (assoc_lane_it == GetContext()->associated_lanes.end()) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "] lane_id[" << obstable_lane_id
           << "] not associated with current stop_sign. skip";
    return -1;
  }

  /* skip the speed check to make it less strick
  auto speed = std::hypot(perception_obstacle.velocity().x(),
                          perception_obstacle.velocity().y());
  if (speed > config_.stop_sign().watch_vehicle_max_valid_stop_speed()) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "] velocity[" << speed << "] not stopped. skip";
    return -1;
  }
  */

  // check a valid stop for stop line of the stop_sign
  auto over_lap_info = assoc_lane_it->second.get()->GetObjectOverlapInfo(
      obstacle_lane.get()->id());
  if (over_lap_info == nullptr) {
    AERROR << "can't find over_lap_info for id: " << obstable_lane_id;
    return -1;
  }
  double stop_line_s = over_lap_info->lane_overlap_info().start_s();
  double obstacle_end_s = obstacle_s + perception_obstacle.length() / 2;
  double distance_to_stop_line = stop_line_s - obstacle_end_s;
  if (distance_to_stop_line > conf_watch_vehicle_max_valid_stop_distance_) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "] distance_to_stop_line[" << distance_to_stop_line
           << "]; stop_line_s" << stop_line_s << "]; obstacle_end_s["
           << obstacle_end_s << "] too far from stop line. skip";
    return -1;
  }

  // use a vector since motocycles/bicycles can be more than one
  std::vector<std::string> vehicles =
      (*watch_vehicles)[obstacle_lane->id().id()];
  if (std::find(vehicles.begin(), vehicles.end(), obstacle_id) ==
      vehicles.end()) {
    ADEBUG << "AddWatchVehicle: lane[" << obstacle_lane->id().id()
           << "] obstacle_id[" << obstacle_id << "]";
    (*watch_vehicles)[obstacle_lane->id().id()].push_back(obstacle_id);
  }

  return 0;
}

/**
 * @brief: remove a watch vehicle which not stopping at stop sign any more
 */
int StopSignUnprotectedStop::RemoveWatchVehicle(
    const PathObstacle& path_obstacle,
    const std::vector<std::string>& watch_vehicle_ids,
    StopSignLaneVehicles* watch_vehicles) {
  CHECK_NOTNULL(watch_vehicles);

  const PerceptionObstacle& perception_obstacle =
      path_obstacle.obstacle()->Perception();
  const std::string& obstacle_id = std::to_string(perception_obstacle.id());
  PerceptionObstacle::Type obstacle_type = perception_obstacle.type();
  std::string obstacle_type_name = PerceptionObstacle_Type_Name(obstacle_type);

  // check if being watched
  if (std::find(watch_vehicle_ids.begin(), watch_vehicle_ids.end(),
                obstacle_id) == watch_vehicle_ids.end()) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "] not being watched. skip";
    return 0;
  }

  // check type
  if (obstacle_type != PerceptionObstacle::UNKNOWN &&
      obstacle_type != PerceptionObstacle::UNKNOWN_MOVABLE &&
      obstacle_type != PerceptionObstacle::BICYCLE &&
      obstacle_type != PerceptionObstacle::VEHICLE) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "]. skip";
    return 0;
  }

  auto point = common::util::MakePointENU(perception_obstacle.position().x(),
                                          perception_obstacle.position().y(),
                                          perception_obstacle.position().z());
  double obstacle_s = 0.0;
  double obstacle_l = 0.0;
  LaneInfoConstPtr obstacle_lane;
  if (HDMapUtil::BaseMap().GetNearestLaneWithHeading(
          point, 5.0, perception_obstacle.theta(), M_PI / 3.0, &obstacle_lane,
          &obstacle_s, &obstacle_l) != 0) {
    ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
           << "]: Failed to find nearest lane from map for position: "
           << point.DebugString()
           << "; heading:" << perception_obstacle.theta();
    return -1;
  }

  bool erase = false;

  bool is_path_cross = !path_obstacle.reference_line_st_boundary().IsEmpty();

  // check obstacle is on an associate lane guarded by stop sign
  const std::string& obstable_lane_id = obstacle_lane.get()->id().id();
  auto assoc_lane_it = std::find_if(
      GetContext()->associated_lanes.begin(),
      GetContext()->associated_lanes.end(),
      [&obstable_lane_id](
          std::pair<LaneInfoConstPtr, OverlapInfoConstPtr>& assc_lane) {
        return assc_lane.first.get()->id().id() == obstable_lane_id;
      });
  if (assoc_lane_it != GetContext()->associated_lanes.end()) {
    // check pass stop line of the stop_sign
    auto over_lap_info = assoc_lane_it->second.get()->GetObjectOverlapInfo(
        obstacle_lane.get()->id());
    if (over_lap_info == nullptr) {
      AERROR << "can't find over_lap_info for id: " << obstable_lane_id;
      return -1;
    }

    double stop_line_end_s = over_lap_info->lane_overlap_info().end_s();
    double obstacle_end_s = obstacle_s + perception_obstacle.length() / 2;
    double distance_pass_stop_line = obstacle_end_s - stop_line_end_s;
    if (distance_pass_stop_line > conf_min_pass_s_distance_ && !is_path_cross) {
      erase = true;

      ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
             << "] distance_pass_stop_line[" << distance_pass_stop_line
             << "] stop_line_end_s[" << stop_line_end_s << "] obstacle_end_s["
             << obstacle_end_s << "] is_path_cross[" << is_path_cross
             << "] passed stop sign, AND path not crosses. "
             << "erase from watch_vehicles";
    }
  } else {
    // passes associated lane (in junction)
    if (!is_path_cross) {
      erase = true;
      ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
             << "] obstable_lane_id[" << obstable_lane_id << "] is_path_cross["
             << is_path_cross
             << "] passed associated lane, AND path not crosses. "
             << "erase from watch_vehicles";
    }
  }

  // check if obstacle stops
  /*
  if (!erase) {
    auto speed = std::hypot(perception_obstacle.velocity().x(),
                            perception_obstacle.velocity().y());
    if (speed > config_.stop_sign().max_watch_vehicle_stop_speed()) {
      ADEBUG << "obstacle_id[" << obstacle_id
          << "] type[" << obstacle_type_name
          << "] velocity[" << speed
          << "] not stopped. erase from watch_vehicles";
      erase = true;
    }
  }
  */

  if (erase) {
    for (StopSignLaneVehicles::iterator it = watch_vehicles->begin();
         it != watch_vehicles->end(); ++it) {
      std::vector<std::string>& vehicles = it->second;
      vehicles.erase(std::remove(vehicles.begin(), vehicles.end(), obstacle_id),
                     vehicles.end());
    }
  }

  return 0;
}

/**
 * @brief: check valid stop_sign stop
 */
bool StopSignUnprotectedPreStop::CheckADCStop(
    const ReferenceLineInfo& reference_line_info) {
  double adc_speed =
      common::VehicleStateProvider::Instance()->linear_velocity();
  if (adc_speed > conf_max_adc_stop_speed_) {
    ADEBUG << "ADC not stopped: speed[" << adc_speed << "]";
    return false;
  }

  // check stop close enough to stop line of the stop_sign
  double adc_front_edge_s = reference_line_info.AdcSlBoundary().end_s();
  double stop_line_start_s = GetContext()->next_stop_sign_overlap.start_s;
  double distance_stop_line_to_adc_front_edge =
      stop_line_start_s - adc_front_edge_s;
  ADEBUG << "distance_stop_line_to_adc_front_edge["
         << distance_stop_line_to_adc_front_edge << "]; stop_line_start_s["
         << stop_line_start_s << "]; adc_front_edge_s[" << adc_front_edge_s
         << "]";

  if (distance_stop_line_to_adc_front_edge > conf_max_valid_stop_distance_) {
    ADEBUG << "not a valid stop. too far from stop line.";
    return false;
  }

  // TODO(all): check no BICYCLE in between.

  return true;
}

}  // namespace stop_sign_protected
}  // namespace scenario
}  // namespace planning
}  // namespace apollo