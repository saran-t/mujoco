// Copyright 2021 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tests for engine/engine_forward.c.

#include "src/engine/engine_forward.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mujoco/mjmodel.h>
#include <mujoco/mujoco.h>
#include "src/engine/engine_io.h"
#include "test/fixture.h"

namespace mujoco {
namespace {

std::vector<mjtNum> AsVector(const mjtNum* array, int n) {
  return std::vector<mjtNum>(array, array + n);
}

static const char* const kEnergyConservingPendulumPath =
    "engine/testdata/derivative/energy_conserving_pendulum.xml";
static const char* const kDampedActuatorsPath =
    "engine/testdata/derivative/damped_actuators.xml";

using ::testing::Pointwise;
using ::testing::DoubleNear;
using ::testing::Ne;
using ForwardTest = MujocoTest;

// --------------------------- activation limits -------------------------------

TEST_F(ForwardTest, ActLimited) {
  static constexpr char xml[] = R"(
  <mujoco>
    <option timestep="0.01"/>
    <worldbody>
      <body>
        <joint name="slide" type="slide" axis="1 0 0"/>
        <geom size=".1"/>
      </body>
    </worldbody>
    <actuator>
      <general joint="slide" gainprm="100" biasprm="0 -100" biastype="affine"
      dynprm="10" dyntype="integrator"
      actlimited="true" actrange="-1 1"/>
    </actuator>
  </mujoco>
  )";

  mjModel* model = LoadModelFromString(xml);
  mjData* data = mj_makeData(model);

  data->ctrl[0] = 1.0;
  // integrating up from 0, we will hit the clamp after 99 steps
  for (int i=0; i<200; i++) {
    mj_step(model, data);
    // always greater than lower bound
    ASSERT_GT(data->act[0], -1);
    // after 99 steps we hit the upper bound
    if (i < 99) ASSERT_LT(data->act[0], 1);
    if (i >= 99) ASSERT_EQ(data->act[0], 1);
  }

  data->ctrl[0] = -1.0;
  // integrating down from 1, we will hit the clamp after 199 steps
  for (int i=0; i<300; i++) {
    mj_step(model, data);
    // always smaller than upper bound
    ASSERT_LT(data->act[0], model->actuator_actrange[1]);
    // after 199 steps we hit the lower bound
    if (i < 199) ASSERT_GT(data->act[0], model->actuator_actrange[0]);
    if (i >= 199) ASSERT_EQ(data->act[0], model->actuator_actrange[0]);
  }

  mj_deleteData(data);
  mj_deleteModel(model);
}

// --------------------------- implicit integrator -----------------------------

using ImplicitIntegratorTest = MujocoTest;

// Euler and implicit should be equivalent if there is only joint damping
TEST_F(ImplicitIntegratorTest, EulerImplicitEqivalent) {
  static constexpr char xml[] = R"(
  <mujoco>
    <worldbody>
      <body>
        <joint axis="1 0 0" damping="2"/>
        <geom type="capsule" size=".01" fromto="0 0 0 0 .1 0"/>
        <body pos="0 .1 0">
          <joint axis="0 1 0" damping="1"/>
          <geom type="capsule" size=".01" fromto="0 0 0 .1 0 0"/>
        </body>
      </body>
    </worldbody>
  </mujoco>
  )";

  mjModel* model = LoadModelFromString(xml);
  mjData* data = mj_makeData(model);

  // step 10 times with Euler, save copy of qpos as vector
  for (int i=0; i<10; i++) {
    mj_step(model, data);
  }
  std::vector<mjtNum> qposEuler = AsVector(data->qpos, model->nq);

  // reset, step 10 times with implicit
  mj_resetData(model, data);
  model->opt.integrator = mjINT_IMPLICIT;
  for (int i=0; i<10; i++) {
    mj_step(model, data);
  }

  // expect qpos vectors to be numerically different
  EXPECT_THAT(AsVector(data->qpos, model->nq), Pointwise(Ne(), qposEuler));

  // expect qpos vectors to be similar to high precision
  EXPECT_THAT(AsVector(data->qpos, model->nq),
              Pointwise(DoubleNear(1e-14), qposEuler));

  mj_deleteData(data);
  mj_deleteModel(model);
}

// Joint and actuator damping should integrate identically under implicit
TEST_F(ImplicitIntegratorTest, JointActuatorEqivalent) {
  const std::string xml_path = GetTestDataFilePath(kDampedActuatorsPath);
  mjModel* model = mj_loadXML(xml_path.c_str(), nullptr, nullptr, 0);
  mjData* data = mj_makeData(model);

  // take 1000 steps with Euler
  for (int i=0; i<1000; i++) {
    mj_step(model, data);
  }
  // expect corresponding joint values to be significantly different
  EXPECT_GT(fabs(data->qpos[0]-data->qpos[2]), 1e-4);
  EXPECT_GT(fabs(data->qpos[1]-data->qpos[3]), 1e-4);

  // reset, take 1000 steps with implicit
  mj_resetData(model, data);
  model->opt.integrator = mjINT_IMPLICIT;
  for (int i=0; i<10; i++) {
    mj_step(model, data);
  }

  // expect corresponding joint values to be insignificantly different
  EXPECT_LT(fabs(data->qpos[0]-data->qpos[2]), 1e-16);
  EXPECT_LT(fabs(data->qpos[1]-data->qpos[3]), 1e-16);

  mj_deleteData(data);
  mj_deleteModel(model);
}

// Energy conservation: RungeKutta > implicit > Euler
TEST_F(ImplicitIntegratorTest, EnergyConservation) {
  const std::string xml_path =
      GetTestDataFilePath(kEnergyConservingPendulumPath);
  mjModel* model = mj_loadXML(xml_path.c_str(), nullptr, nullptr, 0);
  mjData* data = mj_makeData(model);

  const int nstep = 500;  // number of steps to take

  // take nstep steps with Euler, measure energy (potential + kinetic)
  model->opt.integrator = mjINT_EULER;
  for (int i=0; i<nstep; i++) {
    mj_step(model, data);
  }
  mjtNum energyEuler = data->energy[0] + data->energy[1];

  // take nstep steps with implicit, measure energy
  model->opt.integrator = mjINT_IMPLICIT;
  mj_resetData(model, data);
  for (int i=0; i<nstep; i++) {
    mj_step(model, data);
  }
  mjtNum energyImplicit = data->energy[0] + data->energy[1];

  // take nstep steps with 4th order Runge-Kutta, measure energy
  model->opt.integrator = mjINT_RK4;
  mj_resetData(model, data);
  for (int i=0; i<nstep; i++) {
    mj_step(model, data);
  }
  mjtNum energyRK4 = data->energy[0] + data->energy[1];

  // energy was measured: expect all energies to be nonzero
  EXPECT_NE(energyEuler, 0);
  EXPECT_NE(energyImplicit, 0);
  EXPECT_NE(energyRK4, 0);

  // test conservation: perfectly conserved energy would remain 0.0
  // expect RK4 to be better than implicit
  EXPECT_LT(fabs(energyRK4), fabs(energyImplicit));
  // expect implicit to be better than Euler
  EXPECT_LT(fabs(energyImplicit), fabs(energyEuler));

  mj_deleteData(data);
  mj_deleteModel(model);
}

}  // namespace
}  // namespace mujoco
