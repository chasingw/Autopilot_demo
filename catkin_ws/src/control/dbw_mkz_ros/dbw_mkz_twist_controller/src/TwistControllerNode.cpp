/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015-2018, Dataspeed Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Dataspeed Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include "TwistControllerNode.h"

namespace dbw_mkz_twist_controller {

TwistControllerNode::TwistControllerNode(ros::NodeHandle &n, ros::NodeHandle &pn) : srv_(pn)
{
  lpf_fuel_.setParams(60.0, 0.1);
  accel_pid_.setRange(0.0, 1.0);

  // Dynamic reconfigure
  srv_.setCallback(boost::bind(&TwistControllerNode::reconfig, this, _1, _2));

  // Control rate parameter
  double control_rate;
  pn.param("control_rate", control_rate, 50.0);
  control_period_ = 1.0 / control_rate;

  // Ackermann steering parameters
  acker_wheelbase_ = 2.8498; // 112.2 inches
  acker_track_ = 1.5824; // 62.3 inches
  steering_ratio_ = 14.8;
  pn.getParam("ackermann_wheelbase", acker_wheelbase_);
  pn.getParam("ackermann_track", acker_track_);
  pn.getParam("steering_ratio", steering_ratio_);
  yaw_control_.setWheelBase(acker_wheelbase_);
  yaw_control_.setSteeringRatio(steering_ratio_);

  // Subscribers
  sub_twist_ = n.subscribe("cmd_vel", 1, &TwistControllerNode::recvTwist, this);
  sub_twist2_ = n.subscribe("cmd_vel_with_limits", 1, &TwistControllerNode::recvTwist2, this);
  sub_twist3_ = n.subscribe("cmd_vel_stamped", 1, &TwistControllerNode::recvTwist3, this);
  sub_steering_ = n.subscribe("steering_report", 1, &TwistControllerNode::recvSteeringReport, this);
  sub_imu_ = n.subscribe("imu/data_raw", 1, &TwistControllerNode::recvImu, this);
  sub_enable_ = n.subscribe("dbw_enabled", 1, &TwistControllerNode::recvEnable, this);
  sub_fuel_level_ = n.subscribe("fuel_level_report", 1, &TwistControllerNode::recvFuel, this);

  // Publishers
  pub_throttle_ = n.advertise<dbw_mkz_msgs::ThrottleCmd>("throttle_cmd", 1);
  pub_brake_ = n.advertise<dbw_mkz_msgs::BrakeCmd>("brake_cmd", 1);
  pub_steering_ = n.advertise<dbw_mkz_msgs::SteeringCmd>("steering_cmd", 1);

  // Debug
  pub_accel_ = n.advertise<std_msgs::Float64>("filtered_accel", 1);
  pub_req_accel_ = n.advertise<std_msgs::Float64>("req_accel", 1);

  // Timers
  control_timer_ = n.createTimer(ros::Duration(control_period_), &TwistControllerNode::controlCallback, this);
}

void TwistControllerNode::controlCallback(const ros::TimerEvent& event)
{
  if ((event.current_real - cmd_stamp_).toSec() > (10.0 * control_period_)) {
    speed_pid_.resetIntegrator();
    accel_pid_.resetIntegrator();
    return;
  }

  //计算车辆的实时质量,但是公式感觉有问题,应该是cfg_.vehicle_mass + lpf_fuel_.get() * GAS_DENSITY
  double vehicle_mass = cfg_.vehicle_mass + lpf_fuel_.get() / 100.0 * cfg_.fuel_capacity * GAS_DENSITY;
  //vel_error是期望线速度与实时线速度之差值.
  double vel_error = cmd_vel_.twist.linear.x - actual_.linear.x;
  if ((fabs(cmd_vel_.twist.linear.x) < mphToMps(1.0)) || !cfg_.pub_pedals) {
    speed_pid_.resetIntegrator();
  }

  //设置速度范围.
  speed_pid_.setRange(
      -std::min(fabs(cmd_vel_.decel_limit) > 0.0 ? fabs(cmd_vel_.decel_limit) : 9.8,
                cfg_.decel_max > 0.0 ? cfg_.decel_max : 9.8),
       std::min(fabs(cmd_vel_.accel_limit) > 0.0 ? fabs(cmd_vel_.accel_limit) : 9.8,
                cfg_.accel_max > 0.0 ? cfg_.accel_max : 9.8)
  );

  //这里的时间control_period_没有用上,计算了速度差值,似乎不能得到加速度,差值*比例系数,感觉这应该是要降低为0的数
  //此为timer的回调函数,周期即是control_period_,这个差值是每control_period_计算一次,所以速度的差值也可以看成加速度,时间是control_period_
  double accel_cmd = speed_pid_.step(vel_error, control_period_);

  if (cmd_vel_.twist.linear.x <= (double)1e-2) {
    accel_cmd = std::min(accel_cmd, -530 / vehicle_mass / cfg_.wheel_radius);
  } 

  //发布pid之后的速度,调试使用.
  std_msgs::Float64 accel_cmd_msg;
  accel_cmd_msg.data = accel_cmd;
  pub_req_accel_.publish(accel_cmd_msg);

  if (sys_enable_) {
    dbw_mkz_msgs::ThrottleCmd throttle_cmd;
    dbw_mkz_msgs::BrakeCmd brake_cmd;
    dbw_mkz_msgs::SteeringCmd steering_cmd;

    //accel_cmd是当前线速度与期望速度差值乘以比例系数,lpf_accel_.get()是当前速度与上次测量速度差值乘以50,经过低通滤波
    throttle_cmd.enable = true;
    throttle_cmd.pedal_cmd_type = dbw_mkz_msgs::ThrottleCmd::CMD_PERCENT;
    if (accel_cmd >= 0) {
      throttle_cmd.pedal_cmd = accel_pid_.step(accel_cmd - lpf_accel_.get(), control_period_);
    } else {
      accel_pid_.resetIntegrator();
      throttle_cmd.pedal_cmd = 0;
    }

    brake_cmd.enable = true;
    brake_cmd.pedal_cmd_type = dbw_mkz_msgs::BrakeCmd::CMD_TORQUE;
    if (accel_cmd < -cfg_.brake_deadband) {
      brake_cmd.pedal_cmd = -accel_cmd * vehicle_mass * cfg_.wheel_radius;
    } else {
      brake_cmd.pedal_cmd = 0;
    }

    steering_cmd.enable = true;
    //getSteeringWheelAngle参数:期望线速度,期望角速度,实际线速度.
    steering_cmd.steering_wheel_angle_cmd = yaw_control_.getSteeringWheelAngle(cmd_vel_.twist.linear.x, cmd_vel_.twist.angular.z, actual_.linear.x)
        + cfg_.steer_kp * (cmd_vel_.twist.angular.z - actual_.angular.z);

    if (cfg_.pub_pedals) {
      pub_throttle_.publish(throttle_cmd);
      pub_brake_.publish(brake_cmd);
    }

    if (cfg_.pub_steering) {
      pub_steering_.publish(steering_cmd);
    }
  } else {
    speed_pid_.resetIntegrator();
    accel_pid_.resetIntegrator();
  }
}

//设置参数.
void TwistControllerNode::reconfig(ControllerConfig& config, uint32_t level)
{
  cfg_ = config;
  //车的质量=车的质量-燃料容量*燃料密度.
  cfg_.vehicle_mass -= cfg_.fuel_capacity * GAS_DENSITY; // Subtract weight of full gas tank
  //车的质量=车的质量+150(假设人的体重是150kg)
  cfg_.vehicle_mass += 150.0; // Account for some passengers

  //设置外速度控制回路参数:比例增益ki,积分增益kp和微分增益kd为0
  speed_pid_.setGains(cfg_.speed_kp, 0.0, 0.0);
  //设置内速度控制回路参数比例增益ki和积分增益kp,微分增益kd为0
  accel_pid_.setGains(cfg_.accel_kp, cfg_.accel_ki, 0.0);
  //设置最大横向角速度.
  yaw_control_.setLateralAccelMax(cfg_.max_lat_accel);
  //设置滤波时间和采样频率,这样就确定了滤波系数.
  lpf_accel_.setParams(cfg_.accel_tau, 0.02);
}

//更新理想速度cmd_vel_,这三个应该只有一个是有效的.
void TwistControllerNode::recvTwist(const geometry_msgs::Twist::ConstPtr& msg)
{
  cmd_vel_.twist = *msg;
  cmd_vel_.accel_limit = 0;
  cmd_vel_.decel_limit = 0;
  cmd_stamp_ = ros::Time::now();
}

//更新理想速度cmd_vel_
void TwistControllerNode::recvTwist2(const dbw_mkz_msgs::TwistCmd::ConstPtr& msg)
{
  cmd_vel_ = *msg;
  cmd_stamp_ = ros::Time::now();
}

//更新理想速度cmd_vel_
void TwistControllerNode::recvTwist3(const geometry_msgs::TwistStamped::ConstPtr& msg)
{
  cmd_vel_.twist = msg->twist;
  cmd_vel_.accel_limit = 0;
  cmd_vel_.decel_limit = 0;
  cmd_stamp_ = ros::Time::now();
}

//对燃料级别进行滤波.
//低通滤波器第一次调用filt时,其数值应该比较重要,如果和实际值差别较大,其收敛时间比较旧.
void TwistControllerNode::recvFuel(const dbw_mkz_msgs::FuelLevelReport::ConstPtr& msg)
{
  lpf_fuel_.filt(msg->fuel_level);
}

//采样速度msg->speed设置actual_.linear.x,这里对两次速度的差值,先放大50倍,在进行低通滤波.
void TwistControllerNode::recvSteeringReport(const dbw_mkz_msgs::SteeringReport::ConstPtr& msg)
{
  double raw_accel = 50.0 * (msg->speed - actual_.linear.x);
  lpf_accel_.filt(raw_accel);

  std_msgs::Float64 accel_msg;
  accel_msg.data = lpf_accel_.get();
  pub_accel_.publish(accel_msg);

  actual_.linear.x = msg->speed;
}

//从imu获取角速度,设置actual_
void TwistControllerNode::recvImu(const sensor_msgs::Imu::ConstPtr& msg)
{
  actual_.angular.z = msg->angular_velocity.z;
}

//设置控制使能开关:sys_enable_
void TwistControllerNode::recvEnable(const std_msgs::Bool::ConstPtr& msg)
{
  sys_enable_ = msg->data;
}

}

