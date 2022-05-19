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

#include "engine/engine_forward.h"

#include <stddef.h>
#include <stdio.h>

#include <mujoco/mjdata.h>
#include <mujoco/mjmodel.h>
#include "engine/engine_callback.h"
#include "engine/engine_collision_driver.h"
#include "engine/engine_core_constraint.h"
#include "engine/engine_core_smooth.h"
#include "engine/engine_derivative.h"
#include "engine/engine_inverse.h"
#include "engine/engine_io.h"
#include "engine/engine_macro.h"
#include "engine/engine_sensor.h"
#include "engine/engine_solver.h"
#include "engine/engine_support.h"
#include "engine/engine_util_blas.h"
#include "engine/engine_util_errmem.h"
#include "engine/engine_util_misc.h"
#include "engine/engine_util_solve.h"
#include "engine/engine_util_sparse.h"



//--------------------------- check values ---------------------------------------------------------

// check positions, reset if bad
void mj_checkPos(const mjModel* m, mjData* d) {
  for (int i=0; i<m->nq; i++) {
    if (mju_isBad(d->qpos[i])) {
      mj_warning(d, mjWARN_BADQPOS, i);
      mj_resetData(m, d);
      d->warning[mjWARN_BADQPOS].number++;
      d->warning[mjWARN_BADQPOS].lastinfo = i;
      return;
    }
  }
}



// check velocities, reset if bad
void mj_checkVel(const mjModel* m, mjData* d) {
  for (int i=0; i<m->nv; i++) {
    if (mju_isBad(d->qvel[i])) {
      mj_warning(d, mjWARN_BADQVEL, i);
      mj_resetData(m, d);
      d->warning[mjWARN_BADQVEL].number++;
      d->warning[mjWARN_BADQVEL].lastinfo = i;
      return;
    }
  }
}



// check accelerations, reset if bad
void mj_checkAcc(const mjModel* m, mjData* d) {
  for (int i=0; i<m->nv; i++) {
    if (mju_isBad(d->qacc[i])) {
      mj_warning(d, mjWARN_BADQACC, i);
      mj_resetData(m, d);
      d->warning[mjWARN_BADQACC].number++;
      d->warning[mjWARN_BADQACC].lastinfo = i;
      mj_forward(m, d);
      return;
    }
  }
}



//-------------------------- solver components -----------------------------------------------------

// position-dependent computations
void mj_fwdPosition(const mjModel* m, mjData* d) {
  TM_START1;

  TM_START;
  mj_kinematics(m, d);
  mj_comPos(m, d);
  mj_camlight(m, d);
  mj_tendon(m, d);
  mj_transmission(m, d);
  TM_END(mjTIMER_POS_KINEMATICS);

  TM_RESTART;
  mj_crb(m, d);
  mj_factorM(m, d);
  TM_END(mjTIMER_POS_INERTIA);

  TM_RESTART;
  mj_collision(m, d);
  TM_END(mjTIMER_POS_COLLISION);

  TM_RESTART;
  mj_makeConstraint(m, d);
  TM_END(mjTIMER_POS_MAKE);

  TM_RESTART;
  mj_projectConstraint(m, d);
  TM_END(mjTIMER_POS_PROJECT);

  TM_END1(mjTIMER_POSITION);
}



// velocity-dependent computations
void mj_fwdVelocity(const mjModel* m, mjData* d) {
  TM_START;

  // tendon velocity: dense or sparse
  if (mj_isSparse(m)) {
    mju_mulMatVecSparse(d->ten_velocity, d->ten_J, d->qvel, m->ntendon,
                        d->ten_J_rownnz, d->ten_J_rowadr, d->ten_J_colind, NULL);
  } else {
    mju_mulMatVec(d->ten_velocity, d->ten_J, d->qvel, m->ntendon, m->nv);
  }

  // actuator velocity
  mju_mulMatVec(d->actuator_velocity, d->actuator_moment, d->qvel, m->nu, m->nv);

  // standard velocity computations
  mj_comVel(m, d);
  mj_passive(m, d);
  mj_referenceConstraint(m, d);

  // compute qfrc_bias with abbreviated RNE (without acceleration)
  mj_rne(m, d, 0, d->qfrc_bias);

  TM_END(mjTIMER_VELOCITY);
}



// (qpos, qvel, ctrl, act) => (qfrc_actuator, actuator_force, act_dot)
void mj_fwdActuation(const mjModel* m, mjData* d) {
  TM_START;
  int nv = m->nv, nu = m->nu, na = m->na;
  mjtNum gain, bias, tau;
  mjtNum *prm, *moment = d->actuator_moment, *force = d->actuator_force;

  // clear results
  mju_zero(d->qfrc_actuator, nv);
  if (nu) {
    mju_zero(d->actuator_force, nu);
  }

  // check controls, set to 0 if any are bad
  for (int i=0; i<nu; i++) {
    if (mju_isBad(d->ctrl[i])) {
      mj_warning(d, mjWARN_BADCTRL, i);
      mju_zero(d->ctrl, nu);
      break;
    }
  }

  // disabled or no actuation: return
  if (nu==0 || mjDISABLED(mjDSBL_ACTUATION)) {
    return;
  }

  // force = gain .* [ctrl/act] + bias
  for (int i=0; i<nu; i++) {
    // clamp ctrl
    if (m->actuator_ctrllimited[i] && !mjDISABLED(mjDSBL_CLAMPCTRL)) {
      if (d->ctrl[i] < m->actuator_ctrlrange[2*i]) {
        d->ctrl[i] = m->actuator_ctrlrange[2*i];
      } else if (d->ctrl[i] > m->actuator_ctrlrange[2*i+1]) {
        d->ctrl[i] = m->actuator_ctrlrange[2*i+1];
      }
    }

    // extract gain info
    prm = m->actuator_gainprm + mjNGAIN*i;

    // handle according to gain type
    switch (m->actuator_gaintype[i]) {
    case mjGAIN_FIXED:              // fixed gain: prm = gain
      gain = prm[0];
      break;

    case mjGAIN_MUSCLE:             // muscle gain
      gain = mju_muscleGain(d->actuator_length[i],
                            d->actuator_velocity[i],
                            m->actuator_lengthrange+2*i,
                            m->actuator_acc0[i],
                            prm);
      break;

    default:                        // user gain
      if (mjcb_act_gain) {
        gain = mjcb_act_gain(m, d, i);
      } else {
        gain = 1;
      }
    }

    // set force = gain .* [ctrl/act]
    if (m->actuator_dyntype[i]==mjDYN_NONE) {
      force[i] = gain * d->ctrl[i];
    } else {
      force[i] = gain * d->act[i-(nu-na)];
    }

    // extract bias info
    prm = m->actuator_biasprm + mjNBIAS*i;

    // handle according to bias type
    switch (m->actuator_biastype[i]) {
    case mjBIAS_NONE:               // none
      bias = 0.0;
      break;

    case mjBIAS_AFFINE:             // affine: prm = [const, kp, kv]
      bias = prm[0] + prm[1]*d->actuator_length[i] + prm[2]*d->actuator_velocity[i];
      break;

    case mjBIAS_MUSCLE:             // muscle passive force
      bias =  mju_muscleBias(d->actuator_length[i],
                             m->actuator_lengthrange+2*i,
                             m->actuator_acc0[i],
                             prm);
      break;

    default:                        // user bias
      if (mjcb_act_bias) {
        bias = mjcb_act_bias(m, d, i);
      } else {
        bias = 0;
      }
    }

    // add bias
    force[i] += bias;
  }

  // clamp actuator_force
  for (int i=0; i<nu; i++) {
    if (m->actuator_forcelimited[i]) {
      if (force[i]<m->actuator_forcerange[2*i]) {
        force[i] = m->actuator_forcerange[2*i];
      } else if (force[i]>m->actuator_forcerange[2*i+1]) {
        force[i] = m->actuator_forcerange[2*i+1];
      }
    }
  }

  // qfrc_actuator = moment' * force
  mju_mulMatTVec(d->qfrc_actuator, moment, force, nu, nv);

  // act_dot for stateful actuators
  for (int i=nu-na; i<nu; i++) {
    // extract info
    prm = m->actuator_dynprm + i*mjNDYN;
    int j = i-(nu-na);

    // compute act_dot according to dynamics type
    switch (m->actuator_dyntype[i]) {
    case mjDYN_INTEGRATOR:          // simple integrator
      d->act_dot[j] = d->ctrl[i];
      break;

    case mjDYN_FILTER:              // linear filter: prm = tau
      tau = mju_max(mjMINVAL, prm[0]);
      d->act_dot[j] = (d->ctrl[i] - d->act[j]) / tau;
      break;

    case mjDYN_MUSCLE:              // muscle model: prm = (tau_act, tau_deact)
      d->act_dot[j] = mju_muscleDynamics(d->ctrl[i], d->act[j], prm);
      break;

    default:                        // user dynamics
      if (mjcb_act_dyn) {
        d->act_dot[j] = mjcb_act_dyn(m, d, i);
      } else {
        d->act_dot[j] = 0;
      }
    }
  }

  TM_END(mjTIMER_ACTUATION);
}



// add up all non-constraint forces, compute qacc_smooth
void mj_fwdAcceleration(const mjModel* m, mjData* d) {
  TM_START;
  mjMARKSTACK;
  int nv = m->nv;

  // qforce = sum of all non-constraint forces
  mju_sub(d->qfrc_smooth, d->qfrc_passive, d->qfrc_bias, nv);    // qfrc_bias is negative
  mju_addTo(d->qfrc_smooth, d->qfrc_applied, nv);
  mju_addTo(d->qfrc_smooth, d->qfrc_actuator, nv);
  mj_xfrcAccumulate(m, d, d->qfrc_smooth);

  // qacc_smooth = M \ qfr_smooth
  mj_solveM(m, d, d->qacc_smooth, d->qfrc_smooth, 1);

  mjFREESTACK;
  TM_END(mjTIMER_ACCELERATION);
}



// warmstart/init solver
static void warmstart(const mjModel* m, mjData* d) {
  int nv = m->nv, nefc = d->nefc;

  // warmstart with best of (qacc_warmstart, qacc_smooth)
  if (!mjDISABLED(mjDSBL_WARMSTART)) {
    mjMARKSTACK;
    mjtNum* jar = mj_stackAlloc(d, nefc);

    // start with qacc = qacc_warmstart
    mju_copy(d->qacc, d->qacc_warmstart, nv);

    // compute jar(qacc_warmstart)
    mj_mulJacVec(m, d, jar, d->qacc_warmstart);
    mju_subFrom(jar, d->efc_aref, nefc);

    // update constraints, save cost(qacc_warmstart)
    mjtNum cost_warmstart;
    mj_constraintUpdate(m, d, jar, &cost_warmstart, 0);

    // PGS
    if (m->opt.solver==mjSOL_PGS) {
      // cost(force_warmstart)
      mjtNum PGS_warmstart = mju_dot(d->efc_force, d->efc_b, nefc);
      mjtNum* ARf = mj_stackAlloc(d, nefc);
      if (mj_isSparse(m))
        mju_mulMatVecSparse(ARf, d->efc_AR, d->efc_force, nefc,
                            d->efc_AR_rownnz, d->efc_AR_rowadr,
                            d->efc_AR_colind, NULL);
      else {
        mju_mulMatVec(ARf, d->efc_AR, d->efc_force, nefc, nefc);
      }
      PGS_warmstart += 0.5*mju_dot(d->efc_force, ARf, nefc);

      // use zero if better
      if (PGS_warmstart>0) {
        mju_zero(d->efc_force, nefc);
        mju_zero(d->qfrc_constraint, nv);
      }
    }

    // non-PGS
    else {
      // add Gauss to cost(qacc_warmstart)
      mjtNum* Ma = mj_stackAlloc(d, nv);
      mj_mulM(m, d, Ma, d->qacc_warmstart);
      for (int i=0; i<nv; i++) {
        cost_warmstart += 0.5*(Ma[i]-d->qfrc_smooth[i])*(d->qacc_warmstart[i]-d->qacc_smooth[i]);
      }

      // cost(qacc_smooth)
      mjtNum cost_smooth;
      mj_constraintUpdate(m, d, d->efc_b, &cost_smooth, 0);

      // use qacc_smooth if better
      if (cost_warmstart>cost_smooth) {
        mju_copy(d->qacc, d->qacc_smooth, nv);
      }
    }

    mjFREESTACK;
  }

  // coldstart with qacc = qacc_smooth, efc_force = 0
  else {
    mju_copy(d->qacc, d->qacc_smooth, nv);
    mju_zero(d->efc_force, nefc);
  }
}



// compute efc_b, efc_force, qfrc_constraint; update qacc
void mj_fwdConstraint(const mjModel* m, mjData* d) {
  TM_START;
  int nv = m->nv, nefc = d->nefc;

  // no constraints: copy unconstrained acc, clear forces, return
  if (!nefc) {
    mju_copy(d->qacc, d->qacc_smooth, nv);
    mju_copy(d->qacc_warmstart, d->qacc_smooth, nv);
    mju_zero(d->qfrc_constraint, nv);
    d->solver_iter = 0;
    return;
  }

  // compute efc_b = J*qacc_smooth - aref
  mj_mulJacVec(m, d, d->efc_b, d->qacc_smooth);
  mju_subFrom(d->efc_b, d->efc_aref, nefc);

  // warmstart solver
  warmstart(m, d);
  d->solver_iter = 0;

  // run main solver
  switch (m->opt.solver) {
  case mjSOL_PGS:                     // PGS
    mj_solPGS(m, d, m->opt.iterations);
    break;

  case mjSOL_CG:                      // CG
    mj_solCG(m, d, m->opt.iterations);
    break;

  case mjSOL_NEWTON:                  // Newton
    mj_solNewton(m, d, m->opt.iterations);
    break;

  default:
    mju_error_i("Unknown solver type %d", m->opt.solver);
  }

  // save result for next step warmstart
  mju_copy(d->qacc_warmstart, d->qacc, nv);

  // run noslip solver if enabled
  if (m->opt.noslip_iterations>0) {
    mj_solNoSlip(m, d, m->opt.noslip_iterations);
  }

  TM_END(mjTIMER_CONSTRAINT);
}



//-------------------------- integrators  ----------------------------------------------------------

// Euler integrator, semi-implicit in velocity
void mj_Euler(const mjModel* m, mjData* d) {
  int i, nv = m->nv, nM = m->nM;
  mjMARKSTACK;
  mjtNum* saveM = mj_stackAlloc(d, nM);
  mjtNum* saveLD = mj_stackAlloc(d, nM);
  mjtNum* saveLDiagInv = mj_stackAlloc(d, nv);
  mjtNum* saveLDiagSqrtInv = mj_stackAlloc(d, nv);
  mjtNum* qfrc = mj_stackAlloc(d, nv);
  mjtNum* qacc = mj_stackAlloc(d, nv);

  // check for dof damping
  for (i=0; i<nv; i++) {
    if (m->dof_damping[i]>0) {
      break;
    }
  }

  // no damping: explicit velocity integration
  if (i>=nv) {
    mju_addToScl(d->qvel, d->qacc, m->opt.timestep, nv);
  }

  // damping: integrate implicitly
  else {
    // save M and factorization
    mju_copy(saveM, d->qM, nM);
    mju_copy(saveLD, d->qLD, nM);
    mju_copy(saveLDiagInv, d->qLDiagInv, nv);
    mju_copy(saveLDiagSqrtInv, d->qLDiagSqrtInv, nv);

    // add hB to diagonal of M
    for (i=0; i<nv; i++) {
      d->qM[m->dof_Madr[i]] += m->opt.timestep * m->dof_damping[i];
    }

    // factor
    mj_factorM(m, d);

    // solve
    mju_add(qfrc, d->qfrc_smooth, d->qfrc_constraint, nv);
    mj_solveM(m, d, qacc, qfrc, 1);

    // integrate velocity
    mju_addToScl(d->qvel, qacc, m->opt.timestep, nv);

    // restore M and factorization
    mju_copy(d->qM, saveM, nM);
    mju_copy(d->qLD, saveLD, nM);
    mju_copy(d->qLDiagInv, saveLDiagInv, nv);
    mju_copy(d->qLDiagSqrtInv, saveLDiagSqrtInv, nv);
  }

  // update act
  if (m->na) {
    mju_addToScl(d->act, d->act_dot, m->opt.timestep, m->na);

    // clamp activations
    for (i=0; i<m->na; i++) {
      int iu = i + m->nu - m->na;
      if (m->actuator_actlimited[iu]) {
        mjtNum min = m->actuator_actrange[2*iu];
        mjtNum max = m->actuator_actrange[2*iu+1];
        if (d->act[i]<min) {
          d->act[i] = min;
        } else if (d->act[i]>max) {
          d->act[i] = max;
        }
      }
    }
  }

  // update qpos using new qvel
  mj_integratePos(m, d->qpos, d->qvel, m->opt.timestep);

  // advance time
  d->time += m->opt.timestep;

  mjFREESTACK;
}



// RK4 tableau
const mjtNum RK4_A[9] = {
  0.5,    0,      0,
  0,      0.5,    0,
  0,      0,      1
};

const mjtNum RK4_B[4] = {
  1.0/6.0, 1.0/3.0, 1.0/3.0, 1.0/6.0
};


// Runge Kutta explicit order-N integrator
//  (A,B) is the tableau, C is set to row_sum(A)
void mj_RungeKutta(const mjModel* m, mjData* d, int N) {
  int nv = m->nv, nq = m->nq, na = m->na;
  mjtNum h = m->opt.timestep, time = d->time;
  mjtNum C[9], T[9], *X[10], *F[10], *dX;
  const mjtNum* A = (N==4 ? RK4_A : 0);
  const mjtNum* B = (N==4 ? RK4_B : 0);
  mjMARKSTACK;

  // check order
  if (!A) {
    mju_error("Supported RK orders: N=4");
  }

  // allocate space for intermediate solutions
  dX = mj_stackAlloc(d, 2*nv+na);
  for (int i=0; i<N; i++) {
    X[i] = mj_stackAlloc(d, nq+nv+na);
    F[i] = mj_stackAlloc(d, nv+na);
  }

  // precompute C and T;  C,T,A have size (N-1)
  for (int i=1; i<N; i++) {
    // C(i) = sum_j A(i,j)
    C[i-1] = 0;
    for (int j=0; j<i; j++) {
      C[i-1] += A[(i-1)*(N-1)+j];
    }

    // compute T
    T[i-1] = d->time + C[i-1]*h;
  }

  // init X[0], F[0]; mj_forward() was already called
  mju_copy(X[0], d->qpos, nq);
  mju_copy(X[0]+nq, d->qvel, nv);
  mju_copy(F[0], d->qacc, nv);
  if (na) {
    mju_copy(X[0]+nq+nv, d->act, na);
    mju_copy(F[0]+nv, d->act_dot, na);
  }

  // compute the remaining X[i], F[i]
  for (int i=1; i<N; i++) {
    // compute dX
    mju_zero(dX, 2*nv+na);
    for (int j=0; j<i; j++) {
      mju_addToScl(dX, X[j]+nq, A[(i-1)*(N-1)+j], nv);
      mju_addToScl(dX+nv, F[j], A[(i-1)*(N-1)+j], nv+na);
    }

    // compute X[i] = X[0] '+' dX
    mju_copy(X[i], X[0], nq+nv+na);
    mj_integratePos(m, X[i], dX, h);
    mju_addToScl(X[i]+nq, dX+nv, h, nv+na);

    // set X[i], T[i-1] in mjData
    mju_copy(d->qpos, X[i], nq);
    mju_copy(d->qvel, X[i]+nq, nv);
    if (na) {
      mju_copy(d->act, X[i]+nq+nv, na);
    }
    d->time = T[i-1];

    // evaluate F[i]
    mj_forwardSkip(m, d, mjSTAGE_NONE, 1);  // 1: do not recompute sensors and energy
    mju_copy(F[i], d->qacc, nv);
    if (na) {
      mju_copy(F[i]+nv, d->act_dot, na);
    }
  }

  // compute dX for final update (using B instead of A)
  mju_zero(dX, 2*nv+na);
  for (int j=0; j<N; j++) {
    mju_addToScl(dX, X[j]+nq, B[j], nv);
    mju_addToScl(dX+nv, F[j], B[j], nv+na);
  }

  // compute Xfinal
  d->time = time + h;
  mju_copy(d->qpos, X[0], nq+nv+na);
  mj_integratePos(m, d->qpos, dX, h);
  mju_addToScl(d->qvel, dX+nv, h, nv);
  if (na) {
    mju_addToScl(d->act, dX+2*nv, h, na);

    // clamp activations
    for (int i=0; i<m->na; i++) {
      int iu = i + m->nu - m->na;
      if (m->actuator_actlimited[iu]) {
        mjtNum min = m->actuator_actrange[2*iu];
        mjtNum max = m->actuator_actrange[2*iu+1];
        if (d->act[i]<min) {
          d->act[i] = min;
        } else if (d->act[i]>max) {
          d->act[i] = max;
        }
      }
    }
  }

  mjFREESTACK;
}



//-------------------------- top-level API ---------------------------------------------------------

// fully implicit in velocity
void mj_implicit(const mjModel *m, mjData *d) {
  int nv = m->nv;

  mjMARKSTACK;
  mjtNum *qfrc = mj_stackAlloc(d, nv);
  mjtNum *qacc = mj_stackAlloc(d, nv);

  // construct sparse structure in d->D_xxx
  mj_makeMSparse(m, d, d->D_rownnz, d->D_rowadr, d->D_colind);

  // compute analytical derivative qDeriv
  mjd_smooth_vel(m, d);

  // set qLU = qM - dt*qDeriv
  mj_setMSparse(m, d, d->qLU, d->D_rownnz, d->D_rowadr, d->D_colind);
  mju_addToScl(d->qLU, d->qDeriv, -m->opt.timestep, m->nD);

  // factorize qLU, use qacc as scratch space
  mju_factorLUSparse(d->qLU, nv, (int*)qacc, d->D_rownnz, d->D_rowadr, d->D_colind);

  // set qfrc = qfrc_smooth + qfrc_constraint
  mju_add(qfrc, d->qfrc_smooth, d->qfrc_constraint, nv);

  // solve for qacc: (qM - dt*qDeriv) * qacc = qfrc
  mju_solveLUSparse(qacc, d->qLU, qfrc, nv, d->D_rownnz, d->D_rowadr, d->D_colind);

  // update qvel
  mju_addToScl(d->qvel, qacc, m->opt.timestep, nv);

  // update act
  if (m->na) {
    mju_addToScl(d->act, d->act_dot, m->opt.timestep, m->na);
  }

  // update qpos using new qvel
  mj_integratePos(m, d->qpos, d->qvel, m->opt.timestep);

  // advance time
  d->time += m->opt.timestep;

  mjFREESTACK
}



// forward dynamics with skip; skipstage is mjtStage
void mj_forwardSkip(const mjModel* m, mjData* d, int skipstage, int skipsensor) {
  TM_START;

  // position-dependent
  if (skipstage<mjSTAGE_POS) {
    mj_fwdPosition(m, d);
    if (!skipsensor) {
      mj_sensorPos(m, d);
    }
    if (mjENABLED(mjENBL_ENERGY)) {
      mj_energyPos(m, d);
    }
  }

  // velocity-dependent
  if (skipstage<mjSTAGE_VEL) {
    mj_fwdVelocity(m, d);
    if (!skipsensor) {
      mj_sensorVel(m, d);
    }
    if (mjENABLED(mjENBL_ENERGY)) {
      mj_energyVel(m, d);
    }
  }

  // acceleration-dependent
  if (mjcb_control) {
    mjcb_control(m, d);
  }
  mj_fwdActuation(m, d);
  mj_fwdAcceleration(m, d);
  mj_fwdConstraint(m, d);
  if (!skipsensor) {
    mj_sensorAcc(m, d);
  }

  TM_END(mjTIMER_FORWARD);
}



// forward dynamics
void mj_forward(const mjModel* m, mjData* d) {
  mj_forwardSkip(m, d, mjSTAGE_NONE, 0);
}



// advance simulation using control callback
void mj_step(const mjModel* m, mjData* d) {
  TM_START;

  // common to all integrators
  mj_checkPos(m, d);
  mj_checkVel(m, d);
  mj_forward(m, d);
  mj_checkAcc(m, d);

  // compare forward and inverse solutions if enabled
  if (mjENABLED(mjENBL_FWDINV)) {
    mj_compareFwdInv(m, d);
  }

  // use selected integrator
  switch(m->opt.integrator) {
    case mjINT_EULER:
      mj_Euler(m, d);
      break;

    case mjINT_RK4:
      mj_RungeKutta(m, d, 4);
      break;

    case mjINT_IMPLICIT:
      mj_implicit(m, d);
      break;

    default:
      mju_error("Invalid integrator");
  }

  TM_END(mjTIMER_STEP);
}



// advance simulation in two phases: before input is set by user
void mj_step1(const mjModel* m, mjData* d) {
  TM_START;
  mj_checkPos(m, d);
  mj_checkVel(m, d);
  mj_fwdPosition(m, d);
  mj_sensorPos(m, d);
  mj_energyPos(m, d);
  mj_fwdVelocity(m, d);
  mj_sensorVel(m, d);
  mj_energyVel(m, d);
  if (mjcb_control) {
    mjcb_control(m, d);
  }
  TM_END(mjTIMER_STEP);
}


//   >>>>   user can modify ctrl and q/xfrc_applied between step1 and step2   <<<<


// advance simulation in two phases: after input is set by user
void mj_step2(const mjModel* m, mjData* d) {
  TM_START;
  mj_fwdActuation(m, d);
  mj_fwdAcceleration(m, d);
  mj_fwdConstraint(m, d);
  mj_sensorAcc(m, d);
  mj_checkAcc(m, d);

  // compare forward and inverse solutions if enabled
  if (mjENABLED(mjENBL_FWDINV)) {
    mj_compareFwdInv(m, d);
  }

  // integrate with Euler or implicit; RK4 defaults to Euler
  if (m->opt.integrator==mjINT_IMPLICIT) {
    mj_implicit(m, d);
  } else {
    mj_Euler(m, d);
  }

  d->timer[mjTIMER_STEP].number--;
  TM_END(mjTIMER_STEP);
}
