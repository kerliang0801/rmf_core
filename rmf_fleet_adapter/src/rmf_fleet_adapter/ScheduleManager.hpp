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

#ifndef SRC__FULL_CONTROL__SCHEDULEMANAGER_HPP
#define SRC__FULL_CONTROL__SCHEDULEMANAGER_HPP

#include <rmf_traffic/Trajectory.hpp>
#include <rmf_traffic/schedule/Version.hpp>

#include "Listener.hpp"

#include <rmf_traffic_msgs/msg/schedule_conflict.hpp>
#include <rmf_traffic_msgs/srv/submit_trajectories.hpp>
#include <rmf_traffic_msgs/srv/delay_trajectories.hpp>
#include <rmf_traffic_msgs/srv/replace_trajectories.hpp>
#include <rmf_traffic_msgs/srv/erase_trajectories.hpp>
#include <rmf_traffic_msgs/srv/resolve_conflicts.hpp>

#include <rclcpp/node.hpp>

namespace rmf_fleet_adapter {

class FleetAdapterNode;

struct ScheduleConnections
{
  using SubmitTrajectories = rmf_traffic_msgs::srv::SubmitTrajectories;
  using SubmitTrajectoriesClient = rclcpp::Client<SubmitTrajectories>;
  using SubmitTrajectoriesPtr = SubmitTrajectoriesClient::SharedPtr;

  using DelayTrajectories = rmf_traffic_msgs::srv::DelayTrajectories;
  using DelayTrajectoriesClient = rclcpp::Client<DelayTrajectories>;
  using DelayTrajectoriesPtr = DelayTrajectoriesClient::SharedPtr;

  using ReplaceTrajectories = rmf_traffic_msgs::srv::ReplaceTrajectories;
  using ReplaceTrajectoriesClient = rclcpp::Client<ReplaceTrajectories>;
  using ReplaceTrajectoriesPtr = ReplaceTrajectoriesClient::SharedPtr;

  using EraseTrajectories = rmf_traffic_msgs::srv::EraseTrajectories;
  using EraseTrajectoriesClient = rclcpp::Client<EraseTrajectories>;
  using EraseTrajectoriesPtr = EraseTrajectoriesClient::SharedPtr;

  using ResolveConflicts = rmf_traffic_msgs::srv::ResolveConflicts;
  using ResolveConflictsClient = rclcpp::Client<ResolveConflicts>;
  using ResolveConflictsPtr = ResolveConflictsClient::SharedPtr;

  SubmitTrajectoriesPtr submit_trajectories;
  DelayTrajectoriesPtr delay_trajectories;
  ReplaceTrajectoriesPtr replace_trajectories;
  EraseTrajectoriesPtr erase_trajectories;
  ResolveConflictsPtr resolve_conflicts;

  static ScheduleConnections make(rclcpp::Node& node);

  bool ready() const;
};

//==============================================================================
// TODO(MXG): Move this into rmf_traffic_ros2 as a generalized utility class
class ScheduleManager
{
public:

  ScheduleManager(
      ScheduleConnections* connections,
      rmf_traffic_msgs::msg::FleetProperties properties,
      std::function<void()> revision_callback);

  using TrajectorySet = std::vector<rmf_traffic::Trajectory>;
  using ValidTrajectorySet = std::vector<const rmf_traffic::Trajectory*>;

  void push_trajectories(
      const TrajectorySet& trajectories,
      std::function<void()> approval_callback);

  void push_delay(
      const rmf_traffic::Duration duration,
      const rmf_traffic::Time from_time);

  bool waiting() const;

  const std::vector<rmf_traffic::schedule::Version>& ids() const;

  ~ScheduleManager();

private:

  void submit_trajectories(
      const ValidTrajectorySet& trajectories,
      std::function<void()> approval_callback);

  void replace_trajectories(
      const ValidTrajectorySet& trajectories,
      std::function<void()> approval_callback);

  void resolve_trajectories(
      const ValidTrajectorySet& trajectories,
      std::function<void()> approval_callback);

  void erase_trajectories();

  bool process_queues();

  class ConflictListener;

  ScheduleConnections* _connections;
  rmf_traffic_msgs::msg::FleetProperties _properties;
  std::function<void()> _revision_callback;

  std::function<void()> _queued_change;
  std::vector<std::function<void()>> _queued_delays;

  std::vector<rmf_traffic::schedule::Version> _schedule_ids;
  bool _waiting_for_schedule = false;

  std::unique_ptr<ConflictListener> _conflict_listener;
  bool _have_conflict = false;
  rmf_traffic::schedule::Version _last_conflict_version;
  rmf_traffic::schedule::Version _last_revised_version;
};

} // namespace rmf_fleet_adapter


#endif // SRC__FULL_CONTROL__SCHEDULEMANAGER_HPP
