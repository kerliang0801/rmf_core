/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "FleetAdapterNode.hpp"

#include <rmf_traffic/agv/Interpolate.hpp>

#include <rmf_traffic_ros2/StandardNames.hpp>
#include <rmf_traffic_ros2/Time.hpp>
#include <rmf_traffic_ros2/Trajectory.hpp>

#include <rmf_fleet_adapter/StandardNames.hpp>

#include <rclcpp/macros.hpp>
#include <rclcpp/executors.hpp>

#include "../rmf_fleet_adapter/load_param.hpp"

namespace rmf_fleet_adapter {
namespace read_only {

//==============================================================================
std::shared_ptr<FleetAdapterNode> FleetAdapterNode::make()
{
  auto node = std::shared_ptr<FleetAdapterNode>(new FleetAdapterNode);

  node->_connections = ScheduleConnections::make(*node);

  const auto wait_time =
      get_parameter_or_default_time(*node, "discovery_timeout", 10.0);

  node->_delay_threshold =
      get_parameter_or_default_time(*node, "delay_threshold", 5.0);

  const auto stop_time = std::chrono::steady_clock::now() + wait_time;

  while(rclcpp::ok() && std::chrono::steady_clock::now() < stop_time)
  {
    rclcpp::spin_some(node);
    if (node->_connections->ready())
      return node;
  }

  node->_properties.type =
      rmf_traffic_msgs::msg::FleetProperties::TYPE_NO_CONTROL;
  node->_properties.fleet_id = node->_fleet_name;

  RCLCPP_INFO(
        node->get_logger(),
        "Timeout while trying to connect to traffic schedule");
  return nullptr;
}

FleetAdapterNode::ScheduleEntry::ScheduleEntry(FleetAdapterNode* node)
: schedule(node->_connections.get(), node->_properties, [](){}),
  trajectory("")
{
  // Do nothing
}

//==============================================================================
bool FleetAdapterNode::ignore_fleet(const std::string& fleet_name) const
{
  if (!_fleet_name.empty() && fleet_name != _fleet_name)
    return true;

  return false;
}

//==============================================================================
FleetAdapterNode::FleetAdapterNode()
: rclcpp::Node("fleet_adapter"),
  _fleet_name(get_fleet_name_parameter(*this)),
  _traits(get_traits_or_default(*this, 0.7, 0.3, 0.5, 1.5, 0.6))
{
  _fleet_state_subscription =
      create_subscription<FleetState>(
        FleetStateTopicName, rclcpp::SystemDefaultsQoS(),
        [&](FleetState::UniquePtr msg)
  {
    this->fleet_state_update(std::move(msg));
  });
}

//==============================================================================
void FleetAdapterNode::fleet_state_update(FleetState::UniquePtr state)
{
  if (ignore_fleet(state->name))
    return;

  for(const auto& robot : state->robots)
  {
    const auto insertion = _schedule_entries.insert(
          std::make_pair(robot.name, nullptr));

    if (insertion.second)
      submit_robot(robot, insertion.first);
    else
      update_robot(robot, insertion.first);
  }
}

//==============================================================================
void FleetAdapterNode::push_trajectory(
    const RobotState& state,
    const ScheduleEntries::iterator& it)
{
  it->second->path.clear();
  for (const auto& location : state.path)
    it->second->path.push_back(location);

  it->second->trajectory = make_trajectory(state, it->second->sitting);
  it->second->schedule.push_trajectories({it->second->trajectory}, [](){});
}

//==============================================================================
void FleetAdapterNode::submit_robot(
    const RobotState& state,
    const ScheduleEntries::iterator& it)
{
  it->second = std::make_unique<ScheduleEntry>(this);
  push_trajectory(state, it);
}

//==============================================================================
void FleetAdapterNode::update_robot(
    const RobotState& state,
    const ScheduleEntries::iterator& it)
{
  if (it->second->schedule.waiting())
    return;

  if (handle_delay(state, it))
    return;

  push_trajectory(state, it);
}

//==============================================================================
bool FleetAdapterNode::handle_delay(
    const RobotState& state,
    const ScheduleEntries::iterator& it)
{
  auto& entry = *it->second;

  if (entry.path.size() < state.path.size())
  {
    // If the state has more points in its path than what is remembered from
    // before, then it must have a new path that it is following, so sending a
    // delay is not sufficient.
    return false;
  }

  for (std::size_t i=1; i <= state.path.size(); ++i)
  {
    const auto& l_state = state.path[state.path.size()-i];
    const auto& l_entry = entry.path[entry.path.size()-i];

    const Eigen::Vector3d p_state{l_state.x, l_state.y, l_state.yaw};
    const Eigen::Vector3d p_entry{l_entry.x, l_entry.y, l_entry.yaw};

    // TODO(MXG): Make this threshold configurable
    if ((p_state - p_entry).norm() > 1e-8)
      return false;
  }

  entry.path.clear();
  for (const auto& location : state.path)
    entry.path.push_back(location);

  bool sitting = false;
  auto new_trajectory = make_trajectory(state, sitting);

  if (entry.sitting && sitting)
  {
    // Check if the robot is still sitting in the same location.
    const Eigen::Vector3d p_entry =
        entry.trajectory.back().get_finish_position();
    assert((entry.trajectory.front().get_finish_position() - p_entry).norm() < 1e-12);

    const auto& l_state = state.location;
    const Eigen::Vector3d p_state{l_state.x, l_state.y, l_state.yaw};

    // TODO(MXG): Make this threshold configurable
    if ((p_state.block<2,1>(0,0) - p_entry.block<2,1>(0,0)).norm() > 0.05)
      return false;

    // TODO(MXG): Make this threshold configurable
    if (std::abs(p_state[2] - p_entry[2]) > 10.0*M_PI/180.0)
      return false;

    // Every 3 seconds we'll extend the finish time for the trajectory
    // TODO(MXG): Make these parameters configurable
    const auto current_time = rmf_traffic_ros2::convert(state.location.t);
    const auto next_finish_time = current_time + std::chrono::seconds(10);
    const auto delay = next_finish_time - *entry.trajectory.finish_time();
    if (delay > std::chrono::seconds(3))
    {
      entry.trajectory.back().adjust_finish_times(delay);
      entry.schedule.push_delay(delay, current_time);
    }

    return true;
  }
  else if (sitting)
  {
    // The new robot state indicates the robot should be sitting, but it is not
    // already considered to be sitting. Therefore we will kick it back to
    // push_trajectory() to put a new sitting trajectory on the schedule.
    return false;
  }

  const auto time_difference =
      *new_trajectory.finish_time() - *entry.trajectory.finish_time();
  if (std::abs(time_difference.count()) < _delay_threshold.count())
  {
    // The difference between the current finishing time estimate and the
    // previous finishing time estimate is less than the threshold for reporting
    // a delay. This implies that the difference in the estimate may be an
    // artifact of the estimating and not indicative of a real delay.
    //
    // We will keep the current trajectory as it is in the schedule to avoid
    // needless schedule noise.
    return true;
  }

  if (time_difference.count() < 0)
  {
    // The AGV is ahead of schedule. This shouldn't generally happen because
    // there shouldn't be a situation where the robot moves faster than the
    // estimate. We will log this for debugging reference, but otherise ignore
    // it for now.
    RCLCPP_WARN(
          get_logger(),
          "Robot [" + state.name + "] is unexpectedly ["
          + std::to_string(-rmf_traffic::time::to_seconds(time_difference))
          + "] seconds ahead of schedule. This should not happen; the real "
          + "robots should only ever be behind the predicted schedule.");

    // We'll return true to avoid a needless schedule replacement.
    return true;
  }

  // There was a considerable difference between the scheduled finishing time
  // estimate and the latest finishing time estimate, so we will notify the
  // schedule of a delay.
  const auto from_time =
      rmf_traffic_ros2::convert(state.location.t) - time_difference;

  const auto t_it = entry.trajectory.find(from_time);
  if (t_it == entry.trajectory.end())
  {
    // I can't think of a situation where this could happen, so let's report it
    // as an error and debug it later.
    RCLCPP_ERROR(
          get_logger(),
          "BUG: Robot [" + state.name + "] has a delay, but we cannot identify "
          "where its schedule should be pushed back. This should not happen; "
          "please report this.");

    // We'll return false so that the old trajectory can be replaced with the
    // new one, and hopefully everything keeps working okay.
    return false;
  }

  t_it->adjust_finish_times(time_difference);

  entry.schedule.push_delay(time_difference, from_time);

  // Return true to indicate that the delay has been handled.
  return true;
}

//==============================================================================
rmf_traffic::Trajectory FleetAdapterNode::make_trajectory(
    const RobotState& state, bool& is_sitting) const
{
  // TODO(MXG): Account for the multi-floor use case
  const std::string& map_name = state.location.level_name;

  std::vector<Eigen::Vector3d> positions;
  positions.push_back({state.location.x, state.location.y, state.location.yaw});
  for (const auto& location : state.path)
    positions.push_back({location.x, location.y, location.yaw});

  const auto start_time = rmf_traffic_ros2::convert(state.location.t);

  auto trajectory = rmf_traffic::agv::Interpolate::positions(
        map_name, _traits, start_time, positions);

  if (trajectory.size() < 2)
  {
    // If a robot state results in a single-point trajectory, then we should
    // make a temporary sitting trajectory.
    const Eigen::Vector3d p = positions.front();
    rmf_traffic::Trajectory sitting{map_name};
    sitting.insert(
          start_time, _traits.get_profile(), p, Eigen::Vector3d::Zero());

    const auto finish_time = start_time + std::chrono::seconds(10);
    sitting.insert(
          finish_time, _traits.get_profile(), p, Eigen::Vector3d::Zero());

    is_sitting = true;
    return sitting;
  }

  is_sitting = false;

  return trajectory;
}

} // namespace read_only
} // namespace rmf_fleet_adapter
