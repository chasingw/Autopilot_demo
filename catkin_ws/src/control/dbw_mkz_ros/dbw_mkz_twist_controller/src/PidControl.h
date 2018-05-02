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
/*********************************************************************
* PID控制器:
* 功能: 通过调整输出,使得输入差值逐渐减小.输入值有两个,一个是期望值,一个是测量值,也就是输出的反馈值.
* 参数:
  ** kp: 比例系数,大步加速进度.
  ** ki: 积分系数,小步加速进度.
  ** kd: 微分系数,小步阻塞进度.
  ** min_: 调节的最小值输出.
  ** max_: 调节的最大值输出.
  ** int_val_:当前积分值
  ** last_int_val_:上一时刻积分值
  ** error_:当前差值
  ** last_error_:上一时刻差值
  ** sample_time: 采样时间
* 用法:
  ** 调用setParams()函数一次设置参数.
  ** 调用step()函数获取调节之后的值.

**********************************************************************/
#ifndef PIDCONTROL_H
#define PIDCONTROL_H

#include <math.h>

namespace dbw_mkz_twist_controller {

class PidControl {
public:
    PidControl() {
      last_error_ = 0.0; int_val_ = 0.0; last_int_val_ = 0.0;
      kp_ = 0.0; ki_ = 0.0; kd_ = 0.0; min_ = -INFINITY; max_ = INFINITY;
    }
    PidControl(double kp, double ki, double kd, double min, double max) {
      last_error_ = 0.0; int_val_ = 0.0; last_int_val_ = 0.0;
      kp_ = kp; ki_ = ki; kd_ = kd; min_ = std::min(min,max); max_ = std::max(min,max);
    }

    //设置增益三大系数.
    void setGains(double kp, double ki, double kd) { kp_ = kp; ki_ = ki; kd_ = kd; }
    //设置区间,最大值和最小值.
    void setRange(double min, double max) { min_ = std::min(min,max); max_ = std::max(min,max); }
    //设置参数,包含setGains和setRange.
    void setParams(double kp, double ki, double kd, double min, double max) { setGains(kp,ki,kd); setRange(min,max); }
    //reset Integrator积分器,设为0
    void resetIntegrator() { int_val_ = 0.0; last_int_val_ = 0.0; }
    //恢复Integrator积分器,设为last_int_val_
    void revertIntegrator() { int_val_ = last_int_val_; }

    double step(double error, double sample_time) {
      last_int_val_ = int_val_;

      //积分
      double integral = int_val_ + error * sample_time;
      //导数
      double derivative = (error - last_error_) / sample_time;

      double y = kp_ * error + ki_ * int_val_ + kd_ * derivative;
      if (y > max_) {
        y = max_;
      } else if (y < min_) {
        y = min_;
      } else {
        int_val_ = integral;
      }
      last_error_ = error;
      return y;
    }

private:
    double last_error_;
    double int_val_, last_int_val_;
    double kp_, ki_, kd_;
    double min_, max_;
};

}

#endif // PIDCONTROL_H

