/*============================================================================
 * Handle the solidification module with CDO schemes
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2021 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <assert.h>
#include <float.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include <bft_error.h>
#include <bft_mem.h>
#include <bft_printf.h>

#include "cs_cdofb_scaleq.h"
#include "cs_navsto_system.h"
#include "cs_parall.h"
#include "cs_post.h"

/*----------------------------------------------------------------------------
 * Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_solidification.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Local macro definitions
 *============================================================================*/

#define CS_SOLIDIFICATION_DBG       0

static const char _state_names[CS_SOLIDIFICATION_N_STATES][32] = {

  "Solid",
  "Mushy",
  "Liquid",
  "Eutectic"

};

/*============================================================================
 * Static variables
 *============================================================================*/

static cs_real_t  cs_solidification_forcing_eps  = 1e-3;
static cs_real_t  cs_solidification_eutectic_threshold  = 1e-4;
static const double  cs_solidification_diffusion_eps = 1e-16;

/*============================================================================
 * Type definitions
 *============================================================================*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*============================================================================
 * Static global variables
 *============================================================================*/

static const char _err_empty_module[] =
  " Stop execution.\n"
  " The structure related to the solidifcation module is empty.\n"
  " Please check your settings.\n";

static cs_solidification_t  *cs_solidification_structure = NULL;

/*============================================================================
 * Private static inline function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the liquidus temperature knowing the bulk concentration
 *         Assumption of the lever rule.
 *
 * \param[in]  alloy    pointer to a binary alloy structure
 * \param[in]  conc     value of the bulk concentration
 *
 * \return the computed liquidus temperature
 */
/*----------------------------------------------------------------------------*/

static inline cs_real_t
_get_t_liquidus(const cs_solidification_binary_alloy_t     *alloy,
                const cs_real_t                             conc)
{
  return  fmax(alloy->t_eut, alloy->t_melt + alloy->ml * conc);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the solidus temperature knowing the bulk concentration
 *         Assumption of the lever rule.
 *
 * \param[in]  alloy    pointer to a binary alloy structure
 * \param[in]  conc     value of the bulk concentration
 *
 * \return the computed solidus temperature
 */
/*----------------------------------------------------------------------------*/

static inline cs_real_t
_get_t_solidus(const cs_solidification_binary_alloy_t     *alloy,
               const cs_real_t                             conc)
{
  if (conc < alloy->cs1)
    return alloy->t_melt + alloy->ml * conc * alloy->inv_kp;
  else
    return alloy->t_eut;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the value of eta (Cliq = eta * Cbulk) knowing the bulk
 *         concentration and the phase diagram.
 *         Assumption of the lever rule.
 *
 * \param[in] alloy      pointer to a binary alloy structure
 * \param[in] conc       value of the bulk concentration
 *
 * \return the value of eta
 */
/*----------------------------------------------------------------------------*/

static inline cs_real_t
_get_eta(const cs_solidification_binary_alloy_t     *alloy,
         const cs_real_t                             conc)
{
  /* Update eta */
  if (conc > alloy->cs1)
    /* In this case Cl = C_eut = eta * Cbulk--> eta = C_eut/Cbulk */
    return alloy->c_eut/conc;
  else
    return alloy->inv_kp;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Determine in which state is a couple (temp, conc)
 *         Assumption of the lever rule.
 *
 * \param[in]  alloy    pointer to a binary alloy structure
 * \param[in]  temp     value of the temperature
 * \param[in]  conc     value of the bulk concentration
 *
 * \return the state among (liquid, solid, mushy or eutectic)
 */
/*----------------------------------------------------------------------------*/

static inline cs_solidification_state_t
_which_state(const cs_solidification_binary_alloy_t     *alloy,
             const cs_real_t                             temp,
             const cs_real_t                             conc)
{
  const cs_real_t  t_liquidus = _get_t_liquidus(alloy, conc);

  if (temp > t_liquidus)
    return CS_SOLIDIFICATION_STATE_LIQUID;

  else {   /* temp < t_liquidus */

    const cs_real_t  t_solidus = _get_t_solidus(alloy, conc);
    if (temp > t_solidus)
      return CS_SOLIDIFICATION_STATE_MUSHY;

    else { /* temp < t_solidus */

      if (conc < alloy->cs1 || temp < alloy->t_eut_inf)
        return CS_SOLIDIFICATION_STATE_SOLID;
      else
        return CS_SOLIDIFICATION_STATE_EUTECTIC;

    } /* solidus */
  }   /* liquidus */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Determine in which state is a tuple (temp, conc, gl) from the
 *         evaluation of its enthalpy. The calling code has to be sure that the
 *         tuple is consistent.
 *         Assumption of the lever rule.
 *
 * \param[in]  alloy    pointer to a binary alloy structure
 * \param[in]  cp       value of the heat capacity
 * \param[in]  temp     value of the temperature
 * \param[in]  conc     value of the bulk concentration
 * \param[in]  gliq     value of the liquid fraction
 *
 * \return the state among (liquid, solid, mushy or eutectic)
 */
/*----------------------------------------------------------------------------*/

static inline cs_solidification_state_t
_which_state_by_enthalpy(const cs_solidification_binary_alloy_t     *alloy,
                         const cs_real_t                             cp,
                         const cs_real_t                             temp,
                         const cs_real_t                             conc,
                         const cs_real_t                             gliq)
{
  const cs_real_t  h_liq = cp*_get_t_liquidus(alloy, conc) + alloy->latent_heat;
  const cs_real_t  h = cp*temp + gliq*alloy->latent_heat;

  if (h > h_liq)
    return CS_SOLIDIFICATION_STATE_LIQUID;

  else {

    if (conc > alloy->cs1) {    /* Part with eutectic */

      const cs_real_t  h_sol = cp*alloy->t_eut;
      const cs_real_t  gleut = (conc - alloy->cs1)*alloy->dgldC_eut;
      const cs_real_t  h_eut = cp*alloy->t_eut + gleut*alloy->latent_heat;

      if (h > h_eut)
        return CS_SOLIDIFICATION_STATE_MUSHY;
      else if (h > h_sol)
        return CS_SOLIDIFICATION_STATE_EUTECTIC;
      else
        return CS_SOLIDIFICATION_STATE_SOLID;

    }
    else {                      /* Part without eutectic */

      const cs_real_t  h_sol = cp*(alloy->t_melt+alloy->ml*conc*alloy->inv_kp);
      if (h > h_sol)
        return CS_SOLIDIFICATION_STATE_MUSHY;
      else
        return CS_SOLIDIFICATION_STATE_SOLID;

    } /* Eutectic or not that is the question ? */

  } /* Liquid ? */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the derivatives of g_l w.r.t. the temperature and the
 *         bulk concentration when the current state is MUSHY
 *         Assumption of the lever rule.
 *
 * \param[in]  alloy    pointer to a binary alloy structure
 * \param[in]  temp     value of the temperature
 * \param[in]  conc     value of the bulk concentration
 * \param[out] dgldT    value of the derivative of g_l w.r.t. the temperature
 * \param[out] dgldC    value of the derivative of g_l w.r.t. the concentration
 */
/*----------------------------------------------------------------------------*/

static inline void
_get_dgl_mushy(const cs_solidification_binary_alloy_t     *alloy,
               const cs_real_t                             temp,
               const cs_real_t                             conc,
               cs_real_t                                  *dgldT,
               cs_real_t                                  *dgldC)
{
  const double _dTm = temp - alloy->t_melt;
  const double _kml = alloy->ml * alloy->inv_kpm1;

  *dgldT =  _kml * conc/(_dTm*_dTm);
  *dgldC = -_kml / _dTm;
}

/*============================================================================
 * Private function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Create the structure dedicated to the management of the
 *         solidifcation module
 *
 * \return a pointer to a new allocated cs_solidification_t structure
 */
/*----------------------------------------------------------------------------*/

static cs_solidification_t *
_solidification_create(void)
{
  cs_solidification_t  *solid = NULL;

  BFT_MALLOC(solid, 1, cs_solidification_t);

  /* Default initialization */
  solid->model = 0;
  solid->options = 0;
  solid->post_flag = 0;
  solid->verbosity = 1;

  /* Properties */
  solid->mass_density = NULL;
  solid->rho0 = 0.;
  solid->cp0 = 0.;
  solid->viscosity = NULL;

  /* Quantities related to the liquid fraction */
  solid->g_l = NULL;
  solid->g_l_field = NULL;

  /* State related to each cell */
  solid->cell_state = NULL;

  /* Monitoring */
  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++)
    solid->n_g_cells[i] = 0;
  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++)
    solid->state_ratio[i] = 0;

  /* Plot writer related to the solidification process */
  solid->plot_state = NULL;

  /* Structure related to the thermal system solved as a sub-module */
  solid->temperature = NULL;
  solid->thermal_reaction_coef = NULL;
  solid->thermal_reaction_coef_array = NULL;
  solid->thermal_source_term_array = NULL;

  /* Structure cast on-the-fly w.r.t. the modelling choice */
  solid->model_context = NULL;

  /* Quantities/structure related to the forcing term treated as a reaction term
     in the momentum equation */
  solid->forcing_mom = NULL;
  solid->forcing_mom_array = NULL;
  solid->forcing_coef = 0;
  solid->first_cell = -1;

  return solid;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Build the list of (local) solid cells and enforce a zero-velocity
 *         for this selection
 *
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_enforce_solid_cells(const cs_cdo_quantities_t   *quant)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* List of solid cells */
  cs_lnum_t  *solid_cells = NULL;
  BFT_MALLOC(solid_cells, solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID],
             cs_lnum_t);

  cs_lnum_t  n_solid_cells = 0;
  for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {
    if (solid->cell_state[c_id] == CS_SOLIDIFICATION_STATE_SOLID)
      solid_cells[n_solid_cells++] = c_id;
  }

  assert((cs_gnum_t)n_solid_cells
         == solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID]);
  cs_navsto_system_set_solid_cells(n_solid_cells, solid_cells);

  BFT_FREE(solid_cells);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update/initialize the liquid fraction and its related quantities
 *         This corresponds to the Voller and Prakash (87)
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_liquid_fraction_voller(const cs_mesh_t             *mesh,
                               const cs_cdo_connect_t      *connect,
                               const cs_cdo_quantities_t   *quant,
                               const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_voller_t  *v_model
    = (cs_solidification_voller_t *)solid->model_context;

  /* Sanity checks */
  assert(solid->temperature != NULL);
  assert(v_model != NULL);

  cs_real_t  *g_l = solid->g_l_field->val;
  cs_real_t  *temp = solid->temperature->val;
  assert(temp != NULL);

  /* 1./(t_liquidus - t_solidus) = \partial g_l/\partial Temp */
  const cs_real_t  dgldT = 1./(v_model->t_liquidus - v_model->t_solidus);
  const cs_real_t  inv_forcing_eps = 1./cs_solidification_forcing_eps;

  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++) solid->n_g_cells[i] = 0;

  const cs_real_t  dgldT_coef = solid->rho0*v_model->latent_heat*dgldT/ts->dt[0];

  assert(cs_property_is_uniform(solid->viscosity));
  const cs_real_t  viscl0 = cs_property_get_cell_value(0, ts->t_cur,
                                                       solid->viscosity);
  const cs_real_t  forcing_coef = solid->forcing_coef * viscl0;

  for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL) {

      g_l[c_id] = 0;
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;

      solid->cell_state[c_id] = CS_SOLIDIFICATION_STATE_SOLID;
      solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID] += 1;

    }

    /* Update the liquid fraction
     * Update the source term and the reaction coefficient for the thermal
     * system which are arrays */
    else if (temp[c_id] < v_model->t_solidus) {

      g_l[c_id] = 0;
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;

      solid->cell_state[c_id] = CS_SOLIDIFICATION_STATE_SOLID;
      solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID] += 1;

      /* Update the forcing coefficient treated as a property for a reaction
         term in the momentum eq. */
      solid->forcing_mom_array[c_id] = forcing_coef*inv_forcing_eps;

    }
    else if (temp[c_id] > v_model->t_liquidus) {

      g_l[c_id] = 1;
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;

      solid->n_g_cells[CS_SOLIDIFICATION_STATE_LIQUID] += 1;
      solid->cell_state[c_id] = CS_SOLIDIFICATION_STATE_LIQUID;

      /* Update the forcing coefficient treated as a property for a reaction
         term in the momentum eq. */
      solid->forcing_mom_array[c_id] = 0;

    }
    else { /* Mushy zone */

      const cs_real_t  glc = (temp[c_id] - v_model->t_solidus) * dgldT;

      g_l[c_id] = glc;
      solid->thermal_reaction_coef_array[c_id] = dgldT_coef;
      solid->thermal_source_term_array[c_id] =
        dgldT_coef*temp[c_id]*quant->cell_vol[c_id];

      solid->cell_state[c_id] = CS_SOLIDIFICATION_STATE_MUSHY;
      solid->n_g_cells[CS_SOLIDIFICATION_STATE_MUSHY] += 1;

      /* Update the forcing coefficient treated as a property for a reaction
         term in the momentum eq. */
      const cs_real_t  glm1 = 1 - glc;
      solid->forcing_mom_array[c_id] =
        forcing_coef * glm1*glm1/(glc*glc*glc + cs_solidification_forcing_eps);

    }

  } /* Loop on cells */

  /* At this stage, the number of solid cells is a local count
   * Set the enforcement of the velocity for solid cells */
  if (solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID] > 0)
    _enforce_solid_cells(quant);

  /* Parallel synchronization of the number of cells in each state */
  cs_parall_sum(CS_SOLIDIFICATION_N_STATES, CS_GNUM_TYPE, solid->n_g_cells);
}

/*----------------------------------------------------------------------------*
 * Update functions for the binary alloy modelling
 *----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the state associated to each cell in the case of a binary
 *         alloy. No MPI synchronization has to be performed at this stage.
 *
 * \param[in]  connect   pointer to a cs_cdo_connect_t structure
 * \param[in]  quant     pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts        pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_binary_alloy_final_state(const cs_cdo_connect_t      *connect,
                                 const cs_cdo_quantities_t   *quant,
                                 const cs_time_step_t        *ts)
{
  CS_UNUSED(ts);

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  /* Update the cell state (at this stage, one should have converged between
   * the couple (temp, conc) and the liquid fraction */
  const cs_real_t  *t_bulk = solid->temperature->val;
  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *g_l = solid->g_l_field->val;

  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++) solid->n_g_cells[i] = 0;

  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL) {

      solid->cell_state[c_id] = CS_SOLIDIFICATION_STATE_SOLID;
      solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID] += 1;

    }
    else {

      cs_solidification_state_t  state = _which_state_by_enthalpy(alloy,
                                                                  solid->cp0,
                                                                  t_bulk[c_id],
                                                                  c_bulk[c_id],
                                                                  g_l[c_id]);

      solid->cell_state[c_id] = state;
      solid->n_g_cells[state] += 1;

    }

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the darcy term (acting as a penalization) in the momentum
 *         equation and enforce solid cells by set a zero mass flux.
 *         The parallel reduction on the cell state is performed here (not
 *         before to avoid calling the enforcement if no solid cell is locally
 *         detected.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_velocity_forcing(const cs_mesh_t             *mesh,
                         const cs_cdo_connect_t      *connect,
                         const cs_cdo_quantities_t   *quant,
                         const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(connect);

  cs_solidification_t  *solid = cs_solidification_structure;

  /* At this stage, the number of solid cells is a local count
   * Set the enforcement of the velocity for solid cells */
  if (solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID] > 0)
    _enforce_solid_cells(quant);

  /* Parallel synchronization of the number of cells in each state
   * This should be done done now to avoid going to the cell enforcement whereas
   * there is nothing to do locally */
  cs_parall_sum(CS_SOLIDIFICATION_N_STATES, CS_GNUM_TYPE, solid->n_g_cells);

  assert(cs_property_is_uniform(solid->viscosity));
  const cs_real_t  viscl0 = cs_property_get_cell_value(0, ts->t_cur,
                                                       solid->viscosity);
  const cs_real_t  forcing_coef = solid->forcing_coef * viscl0;
  const cs_real_t  *g_l = solid->g_l_field->val;

  /* Set the forcing term in the momentum equation */
  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (g_l[c_id] < 1.) {       /* Not fully liquid */

      const cs_real_t gsc = 1 - g_l[c_id];
      const cs_real_t glc3 = g_l[c_id]*g_l[c_id]*g_l[c_id];

      solid->forcing_mom_array[c_id] =
        forcing_coef * gsc*gsc/(glc3 + cs_solidification_forcing_eps);

    }
    else
      solid->forcing_mom_array[c_id] = 0;

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the concentration of solute in the liquid phase at the cell
 *         center. This value is used in the buoancy term in the momentum
 *         equation.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_clc(const cs_mesh_t             *mesh,
            const cs_cdo_connect_t      *connect,
            const cs_cdo_quantities_t   *quant,
            const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(ts);

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *t_bulk = solid->temperature->val;
  const cs_real_t  *g_l_pre = solid->g_l_field->val_pre;

  cs_real_t  *c_l = alloy->c_l_cells;

  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL) {
      c_l[c_id] = 0.;
      continue;
    }

    const cs_real_t  conc = c_bulk[c_id];
    const cs_real_t  temp = t_bulk[c_id];

    switch (_which_state(alloy, temp, conc)) {

    case CS_SOLIDIFICATION_STATE_SOLID:
      /* If this is the first time that one reaches the solid state for this
       * cell (i.e previously with g_l > 0), then one updates the liquid
       * concentration and one keeps that value */
      if (g_l_pre[c_id] > 0) {
        if (conc < alloy->cs1)
          c_l[c_id] = conc * alloy->inv_kp;
        else
          c_l[c_id] = alloy->c_eut;
      }
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      c_l[c_id] = (temp - alloy->t_melt) * alloy->inv_ml;
      break;

    case CS_SOLIDIFICATION_STATE_LIQUID:
      c_l[c_id] = conc;
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      c_l[c_id] = alloy->c_eut;
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the liquid fraction in each cell
 *         This function reproduces the same process as the one used in the
 *         legacy FV scheme.
 *         This corresponds to the case of a binary alloy model with no
 *         advective source term for the solute transport.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_gl_legacy(const cs_mesh_t             *mesh,
                  const cs_cdo_connect_t      *connect,
                  const cs_cdo_quantities_t   *quant,
                  const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(ts);

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *t_bulk = solid->temperature->val;
  const cs_real_t  *g_l_pre = solid->g_l_field->val_pre;
  cs_real_t        *g_l = solid->g_l_field->val;

  /* Update g_l values in each cell as well as the cell state and the related
     count */
  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    cs_real_t  eta_new, gliq;

    const cs_real_t  eta_old = alloy->eta_coef_array[c_id];
    const cs_real_t  conc = c_bulk[c_id];
    const cs_real_t  temp = t_bulk[c_id];

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    /* Knowing in which part of the phase diagram we are and we then update the
     * value of the liquid fraction: g_l and eta (the coefficient between the
     * concentration in the liquid phase and the bulk concentration */
    switch (_which_state(alloy, temp, conc)) {

    case CS_SOLIDIFICATION_STATE_SOLID:
      gliq = 0.;
      if (g_l_pre[c_id] > 0)    /* Not in a solid state */
        eta_new = _get_eta(alloy, conc);
      else
        eta_new = eta_old;
     break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      gliq = alloy->inv_kpm1* (alloy->kp - alloy->ml*conc/(temp-alloy->t_melt));

      /* Make sure that the liquid fraction remains inside physical bounds */
      gliq = fmin(fmax(0, gliq), 1.);

      eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
      break;

    case CS_SOLIDIFICATION_STATE_LIQUID:
      gliq = 1;
      eta_new = 1;
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      gliq = (conc - alloy->cs1)*alloy->dgldC_eut;

      /* Make sure that the liquid fraction remains inside physical bounds */
      gliq = fmin(fmax(0, gliq), 1.);

      eta_new = _get_eta(alloy, conc);
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

    /* Update the liquid fraction and apply if needed a relaxation */
    if (alloy->gliq_relax > 0)
      g_l[c_id] = (1 - alloy->gliq_relax)*gliq + alloy->gliq_relax*g_l[c_id];
    else
      g_l[c_id] = gliq;

    /* Update eta and apply if needed a relaxation */
    if (alloy->eta_relax > 0)
      alloy->eta_coef_array[c_id] =
        (1-alloy->eta_relax)*eta_new + alloy->eta_relax*eta_old;
    else
      alloy->eta_coef_array[c_id] = eta_new;

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the liquid fraction in each cell
 *         This function reproduces the same process as the one used in the
 *         legacy FV scheme.
 *         This corresponds to the case of a binary alloy model with an
 *         advective source term for the solute transport.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_gl_legacy_ast(const cs_mesh_t             *mesh,
                      const cs_cdo_connect_t      *connect,
                      const cs_cdo_quantities_t   *quant,
                      const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(ts);

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *t_bulk = solid->temperature->val;
  const cs_real_t  *g_l_pre = solid->g_l_field->val_pre;
  cs_real_t        *g_l = solid->g_l_field->val;
  cs_real_t        *c_l = alloy->c_l_cells;

  /* Update g_l values in each cell as well as the cell state and the related
     count */
  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    cs_real_t  gliq = 1;        /* Initialization as liquid */

    const cs_real_t  conc = c_bulk[c_id];
    const cs_real_t  temp = t_bulk[c_id];

    /* Knowing in which part of the phase diagram we are and we then update
     * the value of the liquid fraction: g_l and the concentration of the
     * liquid "solute" c_l */
    switch (_which_state(alloy, temp, conc)) {

    case CS_SOLIDIFICATION_STATE_SOLID:
      gliq = 0.;

      /* If this is the first time that one reaches the solid state for this
       * cell (i.e previously with g_l > 0), then one updates the liquid
       * concentration and one keeps that value */
      if (g_l_pre[c_id] > 0) {
        if (conc < alloy->cs1)
          c_l[c_id] = conc * alloy->inv_kp;
        else
          c_l[c_id] = alloy->c_eut;
      }
     break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      gliq = alloy->inv_kpm1* (alloy->kp - alloy->ml*conc/(temp-alloy->t_melt));
      c_l[c_id] = (temp - alloy->t_melt) * alloy->inv_ml;
      break;

    case CS_SOLIDIFICATION_STATE_LIQUID:
      c_l[c_id] = conc;
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      gliq = (conc - alloy->cs1)*alloy->dgldC_eut;
      c_l[c_id] = alloy->c_eut;
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

    /* Make sure that the liquid fraction remains inside physical bounds */
    gliq = fmin(fmax(0, gliq), 1.);

    /* Relaxation if needed for the liquid fraction */
    if (alloy->gliq_relax > 0)
      g_l[c_id] = (1 - alloy->gliq_relax)*gliq + alloy->gliq_relax*g_l[c_id];
    else
      g_l[c_id] = gliq;

  } /* Loop on cells */

  /* Update c_l at face values */
  const cs_equation_t  *tr_eq = alloy->solute_equation;
  const cs_real_t  *c_bulk_f = cs_equation_get_face_values(tr_eq, false);
  const cs_real_t  *t_bulk_f = alloy->temp_faces;

  for (cs_lnum_t f_id = 0; f_id < quant->n_faces; f_id++) {

    const cs_real_t  conc = c_bulk_f[f_id];
    const cs_real_t  temp = t_bulk_f[f_id];

    /* Knowing in which part of the phase diagram we are, we then update
     * the value of the concentration of the liquid "solute" */
    switch (_which_state(alloy, temp, conc)) {

    case CS_SOLIDIFICATION_STATE_SOLID:
      if (conc < alloy->cs1)
        alloy->c_l_faces[f_id] = conc * alloy->inv_kp;
      else
        alloy->c_l_faces[f_id] = alloy->c_eut;
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      alloy->c_l_faces[f_id] = (temp - alloy->t_melt) * alloy->inv_ml;
      break;

    case CS_SOLIDIFICATION_STATE_LIQUID:
      alloy->c_l_faces[f_id] = conc;
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      alloy->c_l_faces[f_id] = alloy->c_eut;
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for face %ld\n", __func__, (long)f_id);
      break;

    } /* Switch on face state */

  } /* Loop on faces */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the source term for the thermal equation.
 *         This function reproduces the same process as the one used in the
 *         legacy FV scheme. This corresponds to the binary alloy model.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_thm_legacy(const cs_mesh_t             *mesh,
                   const cs_cdo_connect_t      *connect,
                   const cs_cdo_quantities_t   *quant,
                   const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *c_bulk_pre = alloy->c_bulk->val_pre;
  const cs_real_t  *t_bulk_pre = solid->temperature->val_pre;

  const cs_real_t  rhoLovdt = solid->rho0 * alloy->latent_heat/ts->dt[0];

  for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update: 0 by default */

    const cs_real_t  conc = c_bulk[c_id];
    const cs_real_t  conc_pre = c_bulk_pre[c_id];
    const cs_real_t  temp_pre = t_bulk_pre[c_id];

    /* Knowing in which part of the phase diagram we are, then we update
     * the value of the concentration of the liquid "solute" */
    switch (_which_state(alloy, temp_pre, conc_pre)) {

    case CS_SOLIDIFICATION_STATE_SOLID:
    case CS_SOLIDIFICATION_STATE_LIQUID:
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      {
        cs_real_t  dgldC, dgldT;
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
        solid->thermal_source_term_array[c_id] =
          quant->cell_vol[c_id] * rhoLovdt * ( dgldT * temp_pre +
                                               dgldC * (conc_pre - conc) );
      }
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = quant->cell_vol[c_id] *
        rhoLovdt * alloy->dgldC_eut * (conc_pre - conc);
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the liquid fraction in each cell and related quantities.
 *         This corresponds to the case of a binary alloy model with no
 *         advective source term for the solute transport.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_gl_taylor(const cs_mesh_t             *mesh,
                  const cs_cdo_connect_t      *connect,
                  const cs_cdo_quantities_t   *quant,
                  const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(ts);
  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const double  cpovL = solid->cp0/alloy->latent_heat;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *c_bulk_pre = alloy->c_bulk->val_pre;
  const cs_real_t  *t_bulk_pre = solid->temperature->val_pre;
  cs_real_t        *t_bulk = solid->temperature->val;
  cs_real_t        *g_l = solid->g_l_field->val;

  /* Update g_l values in each cell as well as the cell state and the related
     count */
  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    const cs_real_t  conc = c_bulk[c_id];            /* conc_{n+1}^{k+1}  */
    const cs_real_t  temp = t_bulk[c_id];            /* temp_{n+1}^{k+1} */
    const cs_real_t  conc_pre = c_bulk_pre[c_id];
    const cs_real_t  temp_pre = t_bulk_pre[c_id];

    cs_real_t  dgldC, dgldT, gliq, eta_new;
    cs_solidification_state_t  state, state_pre;

    /* gliq, temp and conc iterates may be not related with the gliq(temp, conc)
     * function until convergence is reached. So one needs to be careful. */
    state = _which_state(alloy, temp, conc);
    state_pre = _which_state(alloy, temp_pre, conc_pre);
    eta_new = alloy->eta_coef_array[c_id]; /* avoid a warning */
    gliq = g_l[c_id];                      /* avoid a warning */

    /* Knowing in which part of the phase diagram we are and we then update
     * the value of the liquid fraction: g_l and the concentration of the
     * liquid "solute" */

    switch (state) {

    case CS_SOLIDIFICATION_STATE_SOLID:

      if (state_pre == CS_SOLIDIFICATION_STATE_LIQUID) {

        /* Liquid --> Solid transition */
        const cs_real_t  t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        const cs_real_t  t_star =
          ( cpovL*temp + dgldT*t_liquidus + dgldC*(conc_pre - conc) ) /
          ( cpovL + dgldT );

        t_bulk[c_id] = t_star;

        gliq = 1 + (dgldT*(t_star - t_liquidus) + dgldC*(conc-conc_pre));

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        if (t_star > alloy->t_eut_sup)  /* Mushy or liquid */
          eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
        else  /* Eutectic or solid */
          eta_new = _get_eta(alloy, conc);

      }
      else {
        gliq = 0;
        eta_new = _get_eta(alloy, conc);
      }
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      if (state_pre == CS_SOLIDIFICATION_STATE_LIQUID) {
        /* Liquid --> Mushy transition */
        const cs_real_t  t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        const cs_real_t  t_star =
          ( cpovL*temp + dgldT*t_liquidus + dgldC*(conc_pre - conc) ) /
          ( cpovL + dgldT );

        gliq = 1 + (dgldT*(t_star - t_liquidus) + dgldC*(conc-conc_pre));

        t_bulk[c_id] = t_star;

      }
      else
        gliq = alloy->inv_kpm1 *
          ( alloy->kp - alloy->ml*conc / (temp - alloy->t_melt) );

      /* Make sure that the liquid fraction remains inside physical bounds */
      gliq = fmin(fmax(0, gliq), 1.);

      eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
      break;

    case CS_SOLIDIFICATION_STATE_LIQUID:
      gliq = 1;
      eta_new = 1;
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      if (state_pre == CS_SOLIDIFICATION_STATE_LIQUID) {
        /* Liquid --> Eutectic transition */
        const cs_real_t  t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        const cs_real_t  t_star =
          ( cpovL*temp + dgldT*t_liquidus + dgldC*(conc_pre - conc) ) /
          ( cpovL + dgldT );

        t_bulk[c_id] = t_star;

        gliq = 1 + (dgldT*(t_star - t_liquidus) + dgldC*(conc-conc_pre));

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        if (t_star > alloy->t_eut_inf)
          eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
        else
          eta_new = _get_eta(alloy, conc);

      }
      else {

        const cs_real_t  temp_k = alloy->tk_bulk[c_id];  /* temp_{n+1}^k */

        /* g_l[c_id] is the value at the iterate k */
        gliq = g_l[c_id] + cpovL * (temp_k - alloy->t_eut);

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        /* In this case Cl = C_eut = eta * Cbulk--> eta = C_eut/Cbulk */
        eta_new = _get_eta(alloy, conc);

      }
      break;

    default:
      bft_error(__FILE__, __LINE__, 0, " %s: Invalid state for cell %ld\n",
                __func__, (long)c_id);
      break;

    } /* Switch on cell state */

    /* Update the liquid fraction and apply if needed a relaxation */
    if (alloy->gliq_relax > 0)
      g_l[c_id] = (1 - alloy->gliq_relax)*gliq + alloy->gliq_relax*g_l[c_id];
    else
      g_l[c_id] = gliq;

    /* Update eta and apply if needed a relaxation */
    if (alloy->eta_relax > 0) {
      cs_real_t  eta_old = alloy->eta_coef_array[c_id];
      alloy->eta_coef_array[c_id] =
        (1-alloy->eta_relax)*eta_new + alloy->eta_relax*eta_old;
    }
    else
      alloy->eta_coef_array[c_id] = eta_new;

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the source term for the thermal equation.
 *         This corresponds to the binary alloy model.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_thm_taylor(const cs_mesh_t             *mesh,
                   const cs_cdo_connect_t      *connect,
                   const cs_cdo_quantities_t   *quant,
                   const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  cs_real_t  dgldC, dgldT;
  cs_solidification_state_t  state_k;

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *c_bulk_pre = alloy->c_bulk->val_pre;
  const cs_real_t  *t_bulk_pre = solid->temperature->val_pre;
  const cs_real_t  *g_l_pre = solid->g_l_field->val_pre;

  const cs_real_t  rhoLovdt = solid->rho0 * alloy->latent_heat/ts->dt[0];
  const double  cpovL = solid->cp0/alloy->latent_heat;

  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    const cs_real_t  conc = c_bulk[c_id];
    const cs_real_t  conc_pre = c_bulk_pre[c_id];
    const cs_real_t  temp_pre = t_bulk_pre[c_id];
    const cs_real_t  gliq_pre = g_l_pre[c_id];

    const cs_real_t  rhocvolLovdt = quant->cell_vol[c_id] * rhoLovdt;

    state_k = _which_state(alloy, alloy->tk_bulk[c_id], alloy->ck_bulk[c_id]);

    /* Knowing in which part of the phase diagram we are, then we update
     * the value of the concentration of the liquid "solute" */
    switch (_which_state(alloy, temp_pre, conc_pre)) {

    case CS_SOLIDIFICATION_STATE_LIQUID:
      /* From the knowledge of the previous iteration, try something
         smarter... */
      if (state_k == CS_SOLIDIFICATION_STATE_LIQUID) {
        solid->thermal_reaction_coef_array[c_id] = 0;
        solid->thermal_source_term_array[c_id] = 0;
      }
      else { /* Liquid --> Mushy transition */
        /*      Liquid --> Solid transition */
        /*      Liquid --> Eutectic transition */
        const cs_real_t  t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
        solid->thermal_source_term_array[c_id] = rhocvolLovdt *
          ( dgldT * t_liquidus + dgldC * (conc_pre - conc) );

      }
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      {
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
        solid->thermal_source_term_array[c_id] = rhocvolLovdt *
          ( dgldT * temp_pre + dgldC * (conc_pre - conc) );
      }
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      {
        const cs_real_t  temp_k = alloy->tk_bulk[c_id];  /* temp_{n+1}^k */

        solid->thermal_reaction_coef_array[c_id] = 0;

        /* Estimate the variation of liquid fraction so that the physical
           bounds are satisfied for the liquid fraction) */
        cs_real_t  dgl = cpovL * (temp_k - alloy->t_eut);

        if (dgl + gliq_pre < 0)
          dgl = -gliq_pre;
        else if (dgl + gliq_pre > 1)
          dgl = 1 - gliq_pre;

        solid->thermal_source_term_array[c_id] = rhocvolLovdt * dgl;

      }
      break;

    case CS_SOLIDIFICATION_STATE_SOLID:
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the liquid fraction in each cell and related quantities.
 *         This corresponds to the case of a binary alloy model with no
 *         advective source term for the solute transport.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_gl_path(const cs_mesh_t             *mesh,
                const cs_cdo_connect_t      *connect,
                const cs_cdo_quantities_t   *quant,
                const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  CS_UNUSED(ts);
  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const double  L = alloy->latent_heat;
  const double  cpovL = solid->cp0/L;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *c_bulk_pre = alloy->c_bulk->val_pre;
  cs_real_t        *t_bulk = solid->temperature->val;
  const cs_real_t  *t_bulk_pre = solid->temperature->val_pre;
  cs_real_t        *g_l = solid->g_l_field->val;
  const cs_real_t  *g_l_pre = solid->g_l_field->val_pre;

  /* Update g_l values in each cell as well as the cell state and the related
     count */
  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    const cs_real_t  conc = c_bulk[c_id];            /* conc_{n+1}^{k+1}  */
    const cs_real_t  temp = t_bulk[c_id];            /* temp_{n+1}^{k+1} */
    const cs_real_t  conc_pre = c_bulk_pre[c_id];
    const cs_real_t  temp_pre = t_bulk_pre[c_id];
    const cs_real_t  gliq_pre = g_l_pre[c_id];

    cs_real_t  dgldC, dgldT, gliq, eta_new, t_liquidus, t_solidus;
    cs_real_t  c_star, t_star, dh, dgl;
    cs_solidification_state_t  state, state_pre;

    /* gliq, temp and conc iterates may be not related with the gliq(temp, conc)
     * function until convergence is reached. So one needs to be careful. */
    gliq = gliq_pre; /* default initialization to avoid a warning */
    state = _which_state(alloy, temp, conc);
    state_pre = _which_state(alloy, temp_pre, conc_pre);
    eta_new = alloy->eta_coef_array[c_id];

    /* Knowing in which part of the phase diagram we are and we then update
     * the value of the liquid fraction: g_l and the concentration of the
     * liquid "solute" */

    switch (state) {

    case CS_SOLIDIFICATION_STATE_SOLID:
      /* ============================= */

      switch (state_pre) {
      case CS_SOLIDIFICATION_STATE_LIQUID: /* Liquid --> Solid transition */
        t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        t_star = ( cpovL*temp + 1 + dgldT*t_liquidus + dgldC*(conc_pre-conc) ) /
          ( cpovL + dgldT );

        gliq = 1 + (dgldT*(t_star - t_liquidus) + dgldC*(conc-conc_pre));

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        if (gliq > 0) {

          t_solidus = _get_t_solidus(alloy, conc);
          if (t_star > t_solidus) /* Mushy or liquid */
            eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
          else {
            /* Remain on the solidus line and redefine a new state */
            t_star = t_solidus;
            eta_new = _get_eta(alloy, conc);
          }
        }
        else
          eta_new = _get_eta(alloy, conc);

        t_bulk[c_id] = t_star;
        break;

      case CS_SOLIDIFICATION_STATE_MUSHY: /* Mushy --> Solid transition */
        t_solidus = _get_t_solidus(alloy, conc);
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        /* Variation of enthalpy when considering a mushy zone */
        dh = solid->cp0*(temp - temp_pre) +
          L*(dgldC*(conc-conc_pre) + dgldT*(temp-temp_pre));

        if (conc < alloy->cs1) { /* without eutectic */

          /* Take into account the fact that the variation of gliq is only in
             the mushy zone */
          c_star = conc_pre +
            (dh - solid->cp0*(temp-temp_pre) - dgldT*(t_solidus-temp_pre) )
            / (L*dgldC);

          gliq = gliq_pre + dgldT*(temp-temp_pre) + dgldC*(c_star-conc_pre);

          /* Make sure that the gliq remains inside physical bounds */
          gliq = fmin(fmax(0, gliq), 1.);
          if (gliq > 0) {        /* still in the mushy zone */
            eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
            t_bulk[c_id] = t_solidus + 1e-6;
          }
          else
            eta_new = _get_eta(alloy, conc);

        }
        else {                  /* with eutectic */

          c_star = conc +
            (dh - solid->cp0*(t_solidus-temp_pre)
             - L*(dgldC*(conc-conc_pre) + dgldT*(t_solidus-temp_pre)) )
            / (L*alloy->dgldC_eut);

          if (c_star < alloy->cs1 || c_star > alloy->c_eut) {
            gliq = 0;
            eta_new = _get_eta(alloy, conc);
          }
          else {

            gliq = gliq_pre +
              dgldC*(conc-conc_pre) + dgldT*(t_solidus-temp_pre) +
              alloy->dgldC_eut * (c_star - conc);

            /* Make sure that the gliq remains inside physical bounds */
            gliq = fmin(fmax(0, gliq), 1.);
            if (gliq > 0)         /* remains on the eutectic plateau */
              t_bulk[c_id] = t_solidus;

            eta_new = _get_eta(alloy, c_star);

          } /* Invalid c_star */

        } /* Eutectic transition taken into account */
        break;

      case CS_SOLIDIFICATION_STATE_EUTECTIC: /* Eutectic --> Solid transition */
        _get_dgl_mushy(alloy, alloy->t_eut, conc_pre, &dgldT, &dgldC);

        /* Variation of gl when considering how is implemented the eutectic
           zone */
        dgl = dgldT*(temp-temp_pre) + alloy->dgldC_eut*(conc-conc_pre);
        dh = solid->cp0*(temp -temp_pre) + dgl*L;

        /* If one remains on the eutectic plateau, then the concentration should
           be c_star w.r.t. dh = dgldC_eut * (C* - Cn) since Tk+1 = Tn = Teut */
        c_star = conc_pre + dh/(L*alloy->dgldC_eut);

        if (c_star < alloy->cs1 || c_star > alloy->c_eut) {
          /* In fact the final state is below the eutectic plateau */
          gliq = 0;
          eta_new = _get_eta(alloy, conc);
        }
        else {

          gliq = gliq_pre + alloy->dgldC_eut*(c_star-conc_pre);
          gliq = fmin(fmax(0, gliq), 1.);
          eta_new = _get_eta(alloy, c_star);
          if (gliq > 0)
            t_bulk[c_id] = alloy->t_eut;
        }
        break;

      default: /* Solid --> solid */
        gliq = 0;
        if (gliq_pre > 0) /* Otherwise keep the same value for eta */
          eta_new = _get_eta(alloy, conc);
        break;

      } /* Switch on the previous state */
      break;


    case CS_SOLIDIFICATION_STATE_MUSHY:
      /* ============================= */

      switch (state_pre) {

      case CS_SOLIDIFICATION_STATE_LIQUID: /* Liquid --> Mushy transition */
        t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        t_star = ( cpovL*temp + dgldT*t_liquidus + dgldC*(conc_pre - conc) ) /
          ( cpovL + dgldT );

        gliq = 1 + (dgldT*(t_star-t_liquidus) + dgldC*(conc-conc_pre));

        t_bulk[c_id] = t_star;
        break;

      case CS_SOLIDIFICATION_STATE_MUSHY:
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        gliq = gliq_pre +
          (dgldT*(temp-temp_pre) + dgldC*(conc-conc_pre));
        break;

      default:
        gliq = alloy->inv_kpm1 *
          ( alloy->kp - alloy->ml*conc / (temp - alloy->t_melt) );

      } /* End of switch on the previous state */

      /* Make sure that the liquid fraction remains inside physical bounds */
      gliq = fmin(fmax(0, gliq), 1.);

      eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
      break;


    case CS_SOLIDIFICATION_STATE_LIQUID:
      /* ============================== */
      gliq = 1;
      eta_new = 1;
      break;


    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      /* ================================ */

      switch (state_pre) {

      case CS_SOLIDIFICATION_STATE_LIQUID: /* Liquid --> Eutectic transition */
        t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        t_star = ( cpovL*temp + dgldT*t_liquidus + dgldC*(conc_pre - conc) ) /
          ( cpovL + dgldT );

        t_bulk[c_id] = t_star;

        gliq = 1 + (dgldT*(t_star - t_liquidus) + dgldC*(conc-conc_pre));

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        if (t_star > alloy->t_eut_inf)
          eta_new = 1/( gliq * (1-alloy->kp) + alloy->kp );
        else
          eta_new = _get_eta(alloy, conc);
        break;

      case CS_SOLIDIFICATION_STATE_MUSHY: /* Mushy --> Eutectic transition */
        assert(conc > alloy->cs1);

        /* First part of the path in the mushy zone */
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        gliq = g_l_pre[c_id] +
          alloy->dgldC_eut*(conc - conc_pre) + dgldT*(alloy->t_eut - temp_pre);

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        eta_new = _get_eta(alloy, conc);
        break;

      default: /* eutectic --> eutectic or solid --> eutectic */
        _get_dgl_mushy(alloy, alloy->t_eut, conc_pre, &dgldT, &dgldC);

        /* Variation of gl when considering how is implemented the eutectic
           zone */
        dgl = dgldT*(temp-temp_pre) + alloy->dgldC_eut*(conc-conc_pre);
        dh = solid->cp0*(temp -temp_pre) + dgl*L;

        /* If one remains on the eutectic plateau, then the concentration should
           be c_star w.r.t. dh = dgldC_eut * (C* - Cn) since Tk+1 = Tn = Teut */
        c_star = conc_pre + dh/(L*alloy->dgldC_eut);

        if (c_star < alloy->cs1 || c_star > alloy->c_eut) {

          gliq = (conc - alloy->cs1)*alloy->dgldC_eut;

          /* In this case Cl = C_eut = eta * Cbulk--> eta = C_eut/Cbulk */
          eta_new = _get_eta(alloy, conc);

        }
        else {

          gliq = gliq_pre + alloy->dgldC_eut*(c_star-conc_pre);
          if (gliq > 0)         /* Remains on the eutectic plateau */
            t_bulk[c_id] = alloy->t_eut;

          eta_new = _get_eta(alloy, c_star);

        }

        /* Make sure that the liquid fraction remains inside physical bounds */
        gliq = fmin(fmax(0, gliq), 1.);

        break;

      }
      break;

    default:
      bft_error(__FILE__, __LINE__, 0, " %s: Invalid state for cell %ld\n",
                __func__, (long)c_id);
      break;

    } /* Switch on cell state */

    /* Update the liquid fraction and apply if needed a relaxation */
    if (alloy->gliq_relax > 0)
      g_l[c_id] = (1 - alloy->gliq_relax)*gliq + alloy->gliq_relax*g_l[c_id];
    else
      g_l[c_id] = gliq;

    /* Update eta and apply if needed a relaxation */
    if (alloy->eta_relax > 0) {
      cs_real_t  eta_old = alloy->eta_coef_array[c_id];
      alloy->eta_coef_array[c_id] =
        (1-alloy->eta_relax)*eta_new + alloy->eta_relax*eta_old;
    }
    else
      alloy->eta_coef_array[c_id] = eta_new;

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Update the source term for the thermal equation.
 *         This corresponds to the binary alloy model.
 *
 * \param[in]  mesh       pointer to a cs_mesh_t structure
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_update_thm_path(const cs_mesh_t             *mesh,
                 const cs_cdo_connect_t      *connect,
                 const cs_cdo_quantities_t   *quant,
                 const cs_time_step_t        *ts)
{
  CS_UNUSED(mesh);
  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const cs_real_t  *c_bulk = alloy->c_bulk->val;
  const cs_real_t  *c_bulk_pre = alloy->c_bulk->val_pre;
  const cs_real_t  *t_bulk = solid->temperature->val;
  const cs_real_t  *t_bulk_pre = solid->temperature->val_pre;

  const cs_real_t  rhoLovdt = solid->rho0 * alloy->latent_heat/ts->dt[0];

  for (cs_lnum_t  c_id = 0; c_id < quant->n_cells; c_id++) {

    if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
      continue; /* No update */

    const cs_real_t  conc_kp1 = c_bulk[c_id]; /* Solute transport solved */
    const cs_real_t  conc_k = alloy->ck_bulk[c_id];
    const cs_real_t  temp_k = t_bulk[c_id];

    const cs_real_t  conc_pre = c_bulk_pre[c_id];
    const cs_real_t  temp_pre = t_bulk_pre[c_id];

    const cs_real_t  rhocvolLovdt = quant->cell_vol[c_id] * rhoLovdt;
    cs_real_t  dgldC, dgldT, t_solidus, t_liquidus;

    cs_solidification_state_t  state_k = _which_state(alloy, temp_k, conc_k);

    /* Knowing in which part of the phase diagram we are, then we update
     * the value of the concentration of the liquid "solute" */
    switch (_which_state(alloy, temp_pre, conc_pre)) {

    case CS_SOLIDIFICATION_STATE_LIQUID:
      /* ==============================
       * From the knowledge of the previous iteration, try something smarter...
       */
      switch (state_k) {
      case CS_SOLIDIFICATION_STATE_MUSHY:    /* Liquid --> Mushy */
        t_liquidus = _get_t_liquidus(alloy, conc_pre);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
        solid->thermal_source_term_array[c_id] = rhocvolLovdt *
          ( dgldT * t_liquidus + dgldC * (conc_pre-conc_kp1) );
        break;

      case CS_SOLIDIFICATION_STATE_EUTECTIC: /* Liquid --> Eutectic */
      case CS_SOLIDIFICATION_STATE_SOLID:    /* Liquid --> Solid */
        t_liquidus = _get_t_liquidus(alloy, conc_pre);
        t_solidus = _get_t_solidus(alloy, conc_kp1);

        _get_dgl_mushy(alloy, t_liquidus, conc_pre, &dgldT, &dgldC);

        solid->thermal_reaction_coef_array[c_id] = 0;
        solid->thermal_source_term_array[c_id] = rhocvolLovdt *
          ( dgldT * (t_liquidus-t_solidus) + dgldC * (conc_pre-conc_kp1) );
        break;

      default:                 /* Liquid */
        solid->thermal_reaction_coef_array[c_id] = 0;
        solid->thermal_source_term_array[c_id] = 0;
        break;

      } /* End of swithon the state k */
      break;

    case CS_SOLIDIFICATION_STATE_MUSHY:
      /* ============================= */
      {
        _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);

        switch (state_k) {

        case CS_SOLIDIFICATION_STATE_SOLID:    /* Mushy --> Solid transition */
          solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
          if (conc_kp1 < alloy->cs1)  /* Part without eutectic */
            solid->thermal_source_term_array[c_id] = rhocvolLovdt *
              ( dgldT*temp_pre + dgldC*(conc_pre-conc_kp1) );
          else                        /* Part with eutectic */
            solid->thermal_source_term_array[c_id] = rhocvolLovdt *
              ( dgldT*temp_pre + alloy->dgldC_eut*(conc_pre-conc_kp1) );
          break;

        case CS_SOLIDIFICATION_STATE_EUTECTIC: /* Mushy --> Eutectic */
          assert(conc_kp1 > alloy->cs1);

          /* First part in the mushy zone but not the final part */
          solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
          solid->thermal_source_term_array[c_id] = rhocvolLovdt *
            ( dgldT*temp_pre + alloy->dgldC_eut*(conc_pre-conc_kp1) );
          break;

        default:
          solid->thermal_reaction_coef_array[c_id] = dgldT * rhoLovdt;
          solid->thermal_source_term_array[c_id] = rhocvolLovdt *
            ( dgldT * temp_pre + dgldC * (conc_pre - conc_kp1) );
          break;

        }

      }
      break;

    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      /* ================================ */
      {
        cs_real_t  r_coef = 0;
        cs_real_t  s_coef = alloy->dgldC_eut * (conc_pre - conc_kp1);

        if (solid->options & CS_SOLIDIFICATION_WITH_PENALIZED_EUTECTIC) {
          if (state_k == CS_SOLIDIFICATION_STATE_EUTECTIC ||
              state_k == CS_SOLIDIFICATION_STATE_SOLID) {
            if (conc_kp1 > alloy->cs1 && conc_kp1 < alloy->c_eut) {
              _get_dgl_mushy(alloy, temp_pre, conc_pre, &dgldT, &dgldC);
              r_coef = dgldT * rhoLovdt;
              s_coef += dgldT*alloy->t_eut;
            }
          }
        }

        solid->thermal_reaction_coef_array[c_id] = r_coef;
        solid->thermal_source_term_array[c_id] = rhocvolLovdt * s_coef;

      }
      break;

    case CS_SOLIDIFICATION_STATE_SOLID:
      /* ============================= */
      solid->thermal_reaction_coef_array[c_id] = 0;
      solid->thermal_source_term_array[c_id] = 0;
      break;

    default:
      bft_error(__FILE__, __LINE__, 0,
                " %s: Invalid state for cell %ld\n", __func__, (long)c_id);
      break;

    } /* Switch on cell state */

  } /* Loop on cells */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Function aims at computing the new temperature/bulk concentration
 *         state for the next iteration as well as updating all related
 *         quantities
 *
 * \param[in]      mesh       pointer to a cs_mesh_t structure
 * \param[in]      connect    pointer to a cs_cdo_connect_t structure
 * \param[in]      quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]      time_step  pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_default_binary_coupling(const cs_mesh_t              *mesh,
                         const cs_cdo_connect_t       *connect,
                         const cs_cdo_quantities_t    *quant,
                         const cs_time_step_t         *time_step)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  assert(solid->model & CS_SOLIDIFICATION_MODEL_USE_TEMPERATURE);
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  const size_t  csize = quant->n_cells*sizeof(cs_real_t);
  const cs_equation_t  *c_eq = alloy->solute_equation;
  const cs_equation_t  *t_eq = solid->thermal_sys->thermal_eq;

  cs_real_t  *temp = cs_equation_get_cell_values(t_eq, false);
  cs_real_t  *conc = cs_equation_get_cell_values(c_eq, false);
  cs_real_t  *g_l = solid->g_l_field->val;

  /* Compute the state at t^(n+1) knowing that at state t^(n) */
  if (solid->options & CS_SOLIDIFICATION_USE_EXTRAPOLATION) {

    /* At this stage (i.e. before previous to current: val = n, val_pre = n-1 */
    cs_real_t  *temp_pre = cs_equation_get_cell_values(t_eq, true);
    cs_real_t  *conc_pre = cs_equation_get_cell_values(c_eq, true);

    /* extrapolation at f_{n+1} = 2*f_n - f_{n-1} */
    for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {
      alloy->tx_bulk[c_id] = 2*temp[c_id] - temp_pre[c_id];
      alloy->cx_bulk[c_id] = 2*conc[c_id] - conc_pre[c_id];
    }

  } /* Extrapolation is requested */

  /* Non-linear iterations (k) are also performed to converge on the relation
   * gliq^{k+1} = gliq(temp^{k+1}, conc^{k+1})
   *
   * Cbulk^{0}_{n+1} = Cbulk_{n}
   * Tbulk^{0}_{n+1} = Tbulk_{n}
   * gl^{0}_{n+1} = gl_{n}
   */
  cs_equation_current_to_previous(c_eq);
  cs_equation_current_to_previous(t_eq);
  cs_field_current_to_previous(solid->g_l_field);

  /* At the beginning, field_{n+1}^{k=0} = field_n */
  memcpy(alloy->tk_bulk, temp, csize);
  memcpy(alloy->ck_bulk, conc, csize);

  cs_real_t  delta_temp = 1 + alloy->delta_tolerance;
  cs_real_t  delta_cbulk = 1 + alloy->delta_tolerance;

  alloy->iter = 0;
  while ( ( delta_temp  > alloy->delta_tolerance ||
            delta_cbulk > alloy->delta_tolerance  ) &&
          alloy->iter   < alloy->n_iter_max) {

    /* Solve Cbulk^(k+1)_{n+1} knowing Cbulk^{k}_{n+1}  */
    cs_equation_solve(false,  /* No cur2prev inside a non-linear iterative
                                 process */
                      mesh, alloy->solute_equation);

    /* Update the source term for the thermal equation */
    alloy->update_thm_st(mesh, connect, quant, time_step);

    /* Solve the thermal system */
    cs_thermal_system_compute(false, /* No cur2prev inside a non-linear
                                        iterative process */
                              mesh, time_step, connect, quant);

    /* Update fields and properties which are related to solved variables
     * g_l, state */
    alloy->update_gl(mesh, connect, quant, time_step);

    /* Update the diffusion property related to the solute */
    if (alloy->diff_coef > cs_solidification_diffusion_eps) {

      const double  rho_D = solid->rho0 * alloy->diff_coef;

#     pragma omp parallel for if (quant->n_cells > CS_THR_MIN)
      for (cs_lnum_t i = 0; i < quant->n_cells; i++)
        alloy->diff_pty_array[i] = (g_l[i] > 0) ?
          rho_D * g_l[i] : cs_solidification_diffusion_eps;

    }

    /* Evolution of the temperature and the bulk concentration during this
       iteration */
    delta_temp = -1, delta_cbulk = -1;
    cs_lnum_t  cid_maxt = -1, cid_maxc = -1;
    for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {

      cs_real_t  dtemp = fabs(temp[c_id] - alloy->tk_bulk[c_id]);
      cs_real_t  dconc = fabs(conc[c_id] - alloy->ck_bulk[c_id]);

      alloy->tk_bulk[c_id] = temp[c_id];
      alloy->ck_bulk[c_id] = conc[c_id];

      if (dtemp > delta_temp)
        delta_temp = dtemp, cid_maxt = c_id;
      if (dconc > delta_cbulk)
        delta_cbulk = dconc, cid_maxc = c_id;

    } /* Loop on cells */

    alloy->iter += 1;
    if (solid->verbosity > 0) {
      cs_log_printf(CS_LOG_DEFAULT,
                    "### Solidification.NL: "
                    " k= %d | delta_temp= %5.3e | delta_cbulk= %5.3e\n",
                    alloy->iter, delta_temp, delta_cbulk);
      if (solid->verbosity > 1)
        cs_log_printf(CS_LOG_DEFAULT,
                      "### Solidification.NL: "
                      " k= %d | delta_temp= %7ld | delta_cbulk= %7ld\n",
                      alloy->iter, (long)cid_maxt, (long)cid_maxc);
    }

  } /* while iterating */

    /* Update the liquid concentration of the solute (c_l) */
  alloy->update_clc(mesh, connect, quant, time_step);

  /* The cell state is now updated at this stage. This will be useful for
     the monitoring */
  _update_binary_alloy_final_state(connect, quant, time_step);

  /* Update the forcing term in the momentum equation */
  alloy->update_velocity_forcing(mesh, connect, quant, time_step);

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Perform the monitoring dedicated to the solidifcation module
 *
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 */
/*----------------------------------------------------------------------------*/

static void
_do_monitoring(const cs_cdo_quantities_t   *quant)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  assert(solid->temperature != NULL);

  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++)
    solid->state_ratio[i] = 0;

  for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {

    const cs_real_t  vol_c = quant->cell_vol[c_id];

    switch (solid->cell_state[c_id]) {
    case CS_SOLIDIFICATION_STATE_SOLID:
      solid->state_ratio[CS_SOLIDIFICATION_STATE_SOLID] += vol_c;
      break;
    case CS_SOLIDIFICATION_STATE_LIQUID:
      solid->state_ratio[CS_SOLIDIFICATION_STATE_LIQUID] += vol_c;
      break;
    case CS_SOLIDIFICATION_STATE_MUSHY:
      solid->state_ratio[CS_SOLIDIFICATION_STATE_MUSHY] += vol_c;
      break;
    case CS_SOLIDIFICATION_STATE_EUTECTIC:
      solid->state_ratio[CS_SOLIDIFICATION_STATE_EUTECTIC] += vol_c;
      break;

    default: /* Should not be in this case */
      break;

    } /* End of switch */

  } /* Loop on cells */

  /* Finalize the monitoring step*/
  cs_parall_sum(CS_SOLIDIFICATION_N_STATES, CS_REAL_TYPE, solid->state_ratio);
  const double  inv_voltot = 100./quant->vol_tot;
  for (int i = 0; i < CS_SOLIDIFICATION_N_STATES; i++)
    solid->state_ratio[i] *= inv_voltot;

  cs_log_printf(CS_LOG_DEFAULT,
                "### Solidification monitoring: liquid/mushy/solid states\n"
                "  * Solid    | %6.2f\%% for %9lu cells;\n"
                "  * Mushy    | %6.2f\%% for %9lu cells;\n"
                "  * Liquid   | %6.2f\%% for %9lu cells;\n",
                solid->state_ratio[CS_SOLIDIFICATION_STATE_SOLID],
                solid->n_g_cells[CS_SOLIDIFICATION_STATE_SOLID],
                solid->state_ratio[CS_SOLIDIFICATION_STATE_MUSHY],
                solid->n_g_cells[CS_SOLIDIFICATION_STATE_MUSHY],
                solid->state_ratio[CS_SOLIDIFICATION_STATE_LIQUID],
                solid->n_g_cells[CS_SOLIDIFICATION_STATE_LIQUID]);

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY)
    cs_log_printf(CS_LOG_DEFAULT,
                  "  * Eutectic | %6.2f\%% for %9lu cells;\n",
                  solid->state_ratio[CS_SOLIDIFICATION_STATE_EUTECTIC],
                  solid->n_g_cells[CS_SOLIDIFICATION_STATE_EUTECTIC]);

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the source term for the momentum equation arising from the
 *         Boussinesq approximation. This relies on the prototype associated
 *         to the generic function pointer \ref cs_dof_function_t
 *         Take into account only the variation of temperature.
 *
 * \param[in]      n_elts        number of elements to consider
 * \param[in]      elt_ids       list of elements ids
 * \param[in]      dense_output  perform an indirection in retval or not
 * \param[in]      input         NULL or pointer to a structure cast on-the-fly
 * \param[in, out] retval        result of the function. Must be allocated.
 */
/*----------------------------------------------------------------------------*/

static void
_temp_boussinesq_source_term(cs_lnum_t            n_elts,
                             const cs_lnum_t     *elt_ids,
                             bool                 dense_output,
                             void                *input,
                             cs_real_t           *retval)
{
  /* Sanity checks */
  assert(retval != NULL && input != NULL);

  /* input is a pointer to a structure */
  const cs_source_term_boussinesq_t  *bq = (cs_source_term_boussinesq_t *)input;

  for (cs_lnum_t i = 0; i < n_elts; i++) {

    cs_lnum_t  id = (elt_ids == NULL) ? i : elt_ids[i]; /* cell_id */
    cs_lnum_t  r_id = dense_output ? i : id; /* position in retval */
    cs_real_t  *_r = retval + 3*r_id;

    /* Thermal effect */
    cs_real_t  bq_coef = -bq->beta*(bq->var[id]-bq->var0);

    for (int k = 0; k < 3; k++)
      _r[k] = bq->rho0 * bq_coef * bq->g[k];

  } /* Loop on elements */
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the source term for the momentum equation arising from the
 *         Boussinesq approximation. This relies on the prototype associated
 *         to the generic function pointer \ref cs_dof_function_t
 *         Take into account the variation of temperature and concentration.
 *
 * \param[in]      n_elts        number of elements to consider
 * \param[in]      elt_ids       list of elements ids
 * \param[in]      dense_output  perform an indirection in retval or not
 * \param[in]      input         NULL or pointer to a structure cast on-the-fly
 * \param[in, out] retval        result of the function. Must be allocated.
 */
/*----------------------------------------------------------------------------*/

static void
_temp_conc_boussinesq_source_term(cs_lnum_t            n_elts,
                                  const cs_lnum_t     *elt_ids,
                                  bool                 dense_output,
                                  void                *input,
                                  cs_real_t           *retval)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  assert(retval != NULL && input != NULL && solid != NULL);
  assert(solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY);

  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  /* input is a pointer to a structure */
  const cs_source_term_boussinesq_t  *bq = (cs_source_term_boussinesq_t *)input;
  const cs_real_t  beta_c = alloy->dilatation_coef;
  const cs_real_t  *c_l = alloy->c_l_cells;

  for (cs_lnum_t i = 0; i < n_elts; i++) {

    cs_lnum_t  id = (elt_ids == NULL) ? i : elt_ids[i]; /* cell_id */
    cs_lnum_t  r_id = dense_output ? i : id;            /* position in retval */
    cs_real_t  *_r = retval + 3*r_id;

    /* Thermal effect */
    const cs_real_t  coef_t = -bq->beta*(bq->var[id] - bq->var0);

    /* Concentration effect */
    const cs_real_t  coef_c = -beta_c*(c_l[id] - alloy->ref_concentration);

    const cs_real_t  coef = bq->rho0*(coef_t + coef_c);
    for (int k = 0; k < 3; k++)
      _r[k] = coef * bq->g[k];

  } /* Loop on elements */
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief   Add a source term to the solute equation derived from an explicit
 *          use of the advective and diffusive operator
 *          Generic function prototype for a hook during the cellwise building
 *          of the linear system
 *          Fit the cs_equation_user_hook_t prototype. This function may be
 *          called by different OpenMP threads
 *
 * \param[in]      eqp         pointer to a cs_equation_param_t structure
 * \param[in]      eqb         pointer to a cs_equation_builder_t structure
 * \param[in]      eqc         context to cast for this discretization
 * \param[in]      cm          pointer to a cellwise view of the mesh
 * \param[in, out] mass_hodge  pointer to a cs_hodge_t structure (mass matrix)
 * \param[in, out] diff_hodge  pointer to a cs_hodge_t structure (diffusion)
 * \param[in, out] csys        pointer to a cellwise view of the system
 * \param[in, out] cb          pointer to a cellwise builder
 */
/*----------------------------------------------------------------------------*/

static void
_fb_solute_source_term(const cs_equation_param_t     *eqp,
                       const cs_equation_builder_t   *eqb,
                       const void                    *eq_context,
                       const cs_cell_mesh_t          *cm,
                       cs_hodge_t                    *mass_hodge,
                       cs_hodge_t                    *diff_hodge,
                       cs_cell_sys_t                 *csys,
                       cs_cell_builder_t             *cb)
{
  CS_UNUSED(mass_hodge);
  CS_UNUSED(eqb);

  if (cb->cell_flag & CS_FLAG_SOLID_CELL)
    return; /* No solute evolution in permanent solid zone */

  const cs_cdofb_scaleq_t  *eqc = (const cs_cdofb_scaleq_t *)eq_context;

  cs_solidification_t  *solid = cs_solidification_structure;
  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  cs_real_t  *cl_c = alloy->c_l_cells;
  cs_real_t  *cl_f = alloy->c_l_faces;

  /* Diffusion part of the source term to add */
  cs_hodge_set_property_value_cw(cm, cb->t_pty_eval, cb->cell_flag,
                                 diff_hodge);

  /* Define the local stiffness matrix: local matrix owned by the cellwise
     builder (store in cb->loc) */
  eqc->get_stiffness_matrix(cm, diff_hodge, cb);

  /* Build the cellwise array: c - c_l
     One should have c_l >= c. Therefore, one takes fmin(...,0) */
  for (short int f = 0; f < cm->n_fc; f++)
    cb->values[f] = fmin(csys->val_n[f] - cl_f[cm->f_ids[f]], 0);
  cb->values[cm->n_fc] = fmin(csys->val_n[cm->n_fc] - cl_c[cm->c_id], 0);

  /* Update the RHS with the diffusion contribution */
  cs_sdm_update_matvec(cb->loc, cb->values, csys->rhs);

  /* Define the local advection matrix */
  eqc->advection_build(eqp, cm, csys, eqc->advection_scheme, cb);

  /* Build the cellwise array: c - c_l
     One should have c_l >= c. Therefore, one takes fmin(...,0) */
  for (short int f = 0; f < cm->n_fc; f++)
    cb->values[f] = fmin(csys->val_n[f] - cl_f[cm->f_ids[f]], 0);
  cb->values[cm->n_fc] = fmin(csys->val_n[cm->n_fc] - cl_c[cm->c_id], 0);

  /* Update the RHS with the convection contribution */
  cs_sdm_update_matvec(cb->loc, cb->values, csys->rhs);
}

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Test if solidification module is activated
 */
/*----------------------------------------------------------------------------*/

bool
cs_solidification_is_activated(void)
{
  if (cs_solidification_structure == NULL)
    return false;
  else
    return true;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Retrieve the main structure to deal with solidification process
 *
 * \return a pointer to a new allocated solidification structure
 */
/*----------------------------------------------------------------------------*/

cs_solidification_t *
cs_solidification_get_structure(void)
{
  return cs_solidification_structure;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the level of verbosity for the solidification module
 *
 * \param[in]   verbosity     level of verbosity to set
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_verbosity(int   verbosity)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  if (solid == NULL)
    return;

  solid->verbosity = verbosity;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Activate the solidification module
 *
 * \param[in]  model            type of modelling
 * \param[in]  options          flag to handle optional parameters
 * \param[in]  post_flag        predefined post-processings
 * \param[in]  boundaries       pointer to the domain boundaries
 * \param[in]  ns_model         model equations for the NavSto system
 * \param[in]  ns_model_flag    option flag for the Navier-Stokes system
 * \param[in]  algo_coupling    algorithm used for solving the NavSto system
 * \param[in]  ns_post_flag     predefined post-processings for Navier-Stokes
 *
 * \return a pointer to a new allocated solidification structure
 */
/*----------------------------------------------------------------------------*/

cs_solidification_t *
cs_solidification_activate(cs_solidification_model_t       model,
                           cs_flag_t                       options,
                           cs_flag_t                       post_flag,
                           const cs_boundary_t            *boundaries,
                           cs_navsto_param_model_t         ns_model,
                           cs_navsto_param_model_flag_t    ns_model_flag,
                           cs_navsto_param_coupling_t      algo_coupling,
                           cs_navsto_param_post_flag_t     ns_post_flag)
{
  if (model < 1)
    bft_error(__FILE__, __LINE__, 0,
              " %s: Invalid modelling. Model = %d\n", __func__, model);

  /* Allocate an empty structure */
  cs_solidification_t  *solid = _solidification_create();

  /* Set members of the structure according to the given settings */
  solid->model = model;
  solid->options = options;
  if (post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS)
    post_flag |= CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE;
  solid->post_flag = post_flag;

  /* Activate and default settings for the Navier-Stokes module */
  /* ---------------------------------------------------------- */

  ns_model_flag |= CS_NAVSTO_MODEL_SOLIDIFICATION_BOUSSINESQ;

  /* Activate the Navier-Stokes module */
  cs_navsto_system_t  *ns = cs_navsto_system_activate(boundaries,
                                                      ns_model,
                                                      ns_model_flag,
                                                      algo_coupling,
                                                      ns_post_flag);

  solid->mass_density = ns->param->mass_density;
  assert(solid->mass_density != NULL);

  solid->viscosity = ns->param->tot_viscosity;
  assert(solid->viscosity != NULL);

  /* Activate and default settings for the thermal module */
  /* ---------------------------------------------------- */

  cs_flag_t  thm_num = 0, thm_post = 0;
  cs_flag_t  thm_model = CS_THERMAL_MODEL_NAVSTO_ADVECTION;

  if (model & CS_SOLIDIFICATION_MODEL_USE_TEMPERATURE)
    thm_model |= CS_THERMAL_MODEL_USE_TEMPERATURE;
  else if (model & CS_SOLIDIFICATION_MODEL_USE_ENTHALPY)
    thm_model |= CS_THERMAL_MODEL_USE_ENTHALPY;
  else { /* Defined a default choice*/
    thm_model |= CS_THERMAL_MODEL_USE_TEMPERATURE;
    solid->model |= CS_SOLIDIFICATION_MODEL_USE_TEMPERATURE;
  }

  solid->thermal_sys = cs_thermal_system_activate(thm_model, thm_num, thm_post);

  if (thm_model & CS_THERMAL_MODEL_USE_TEMPERATURE) {

    /* Add reaction property for the temperature equation */
    solid->thermal_reaction_coef = cs_property_add("thermal_reaction_coef",
                                                   CS_PROPERTY_ISO);

    /* If liquid, this coefficient is equal to zero */
    cs_property_set_reference_value(solid->thermal_reaction_coef, 0);

    cs_equation_param_t  *th_eqp =
      cs_equation_get_param(solid->thermal_sys->thermal_eq);

    cs_equation_add_reaction(th_eqp, solid->thermal_reaction_coef);

  }

  /* Add properties related to this module */
  solid->forcing_mom = cs_property_add("forcing_momentum_coef",
                                       CS_PROPERTY_ISO);

  /* If liquid, this coefficient is equal to zero */
  cs_property_set_reference_value(solid->forcing_mom, 0);

  solid->g_l = cs_property_add("liquid_fraction", CS_PROPERTY_ISO);

  /* Allocate the structure storing the modelling context/settings */

  if (solid->model & CS_SOLIDIFICATION_MODEL_VOLLER_PRAKASH_87) {

    cs_solidification_voller_t  *v_model = NULL;
    BFT_MALLOC(v_model, 1, cs_solidification_voller_t);
    solid->model_context = (void *)v_model;

  }

  else if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy = NULL;
    BFT_MALLOC(alloy, 1, cs_solidification_binary_alloy_t);
    solid->model_context = (void *)alloy;

  }

  /* Set the global pointer */
  cs_solidification_structure = solid;

  return solid;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the value of the epsilon parameter used in the forcing term
 *         of the momemtum equation
 *
 * \param[in]  forcing_eps    epsilon used in the penalization term to avoid a
 *                            division by zero
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_forcing_eps(cs_real_t    forcing_eps)
{
  assert(forcing_eps > 0);
  cs_solidification_forcing_eps = forcing_eps;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the main physical parameters which described the Voller and
 *         Prakash modelling
 *
 * \param[in]  t_solidus      solidus temperature (in K)
 * \param[in]  t_liquidus     liquidus temperatur (in K)
 * \param[in]  latent_heat    latent heat
 * \param[in]  s_das          secondary dendrite space arms
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_voller_model(cs_real_t    t_solidus,
                                   cs_real_t    t_liquidus,
                                   cs_real_t    latent_heat,
                                   cs_real_t    s_das)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  if ((solid->model & CS_SOLIDIFICATION_MODEL_VOLLER_PRAKASH_87) == 0)
    bft_error(__FILE__, __LINE__, 0,
              " %s: Voller and Prakash model not declared during the"
              " activation of the solidification module.\n"
              " Please check your settings.", __func__);

  cs_solidification_voller_t  *v_model
    = (cs_solidification_voller_t *)solid->model_context;
  assert(v_model != NULL);

  /* Model parameters */
  v_model->t_solidus = t_solidus;
  v_model->t_liquidus = t_liquidus;
  v_model->latent_heat = latent_heat;
  v_model->s_das = s_das;
  if (s_das < FLT_MIN)
    bft_error(__FILE__, __LINE__, 0,
              " %s: Invalid value %g for the secondary dendrite arms spacing",
              __func__, s_das);

  solid->forcing_coef = 180./(s_das*s_das);

  /* Update properties */
  v_model->update = _update_liquid_fraction_voller;

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the main physical parameters which described a solidification
 *         process with a binary alloy (with component A and B)
 *         Add a transport equation for the solute concentration to simulate
 *         the conv/diffusion of the alloy ratio between the two components of
 *         the alloy
 *
 * \param[in]  name          name of the binary alloy
 * \param[in]  varname       name of the unknown related to the tracer eq.
 * \param[in]  conc0         reference mixture concentration
 * \param[in]  beta          solutal dilatation coefficient
 * \param[in]  kp            value of the distribution coefficient
 * \param[in]  mliq          liquidus slope for the solute concentration
 * \param[in]  t_eutec       temperature at the eutectic point
 * \param[in]  t_melt        phase-change temperature for the pure material (A)
 * \param[in]  solute_diff   solutal diffusion coefficient in the liquid
 * \param[in]  latent_heat   latent heat
 * \param[in]  s_das         secondary dendrite arm spacing
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_binary_alloy_model(const char     *name,
                                         const char     *varname,
                                         cs_real_t       conc0,
                                         cs_real_t       beta,
                                         cs_real_t       kp,
                                         cs_real_t       mliq,
                                         cs_real_t       t_eutec,
                                         cs_real_t       t_melt,
                                         cs_real_t       solute_diff,
                                         cs_real_t       latent_heat,
                                         cs_real_t       s_das)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  /* Sanity checks */
  assert(name != NULL && varname != NULL);
  assert(solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY);
  assert(alloy != NULL);
  assert(kp > 0);

  alloy->solute_equation = cs_equation_add(name, varname,
                                           CS_EQUATION_TYPE_SOLIDIFICATION,
                                           1,
                                           CS_PARAM_BC_HMG_NEUMANN);
  alloy->c_bulk = NULL;  /* variable field related to this equation */

  /* Set an upwind scheme by default since it could be a pure advection eq. */
  cs_equation_param_t  *eqp = cs_equation_get_param(alloy->solute_equation);

  /* Set the default numerical options that should be used */
  cs_equation_set_param(eqp, CS_EQKEY_SPACE_SCHEME, "cdo_fb");
  cs_equation_set_param(eqp, CS_EQKEY_HODGE_DIFF_ALGO, "cost");
  cs_equation_set_param(eqp, CS_EQKEY_HODGE_DIFF_COEF, "sushi");
  cs_equation_set_param(eqp, CS_EQKEY_ADV_SCHEME, "upwind");
  cs_equation_set_param(eqp, CS_EQKEY_ADV_FORMULATION, "conservative");

  alloy->c_l_cells = NULL;
  alloy->c_l_faces = NULL;
  alloy->temp_faces = NULL;

  /* Set the main physical parameters */
  alloy->dilatation_coef = beta;
  alloy->ref_concentration = conc0;

  alloy->eta_coef_array = NULL;
  alloy->eta_coef_pty = NULL;

  /* Always add a diffusion term (to avoid a zero block face-face when there
     is no more convection */
  if (solute_diff > 0)
    alloy->diff_coef = solute_diff;
  else
    alloy->diff_coef = cs_solidification_diffusion_eps;

  char  *pty_name = NULL;
  int  len = strlen(varname) + strlen("_diff_pty") + 1;
  BFT_MALLOC(pty_name, len, char);
  sprintf(pty_name, "%s_diff_pty", varname);
  alloy->diff_pty = cs_property_add(pty_name, CS_PROPERTY_ISO);
  BFT_FREE(pty_name);

  cs_equation_add_diffusion(eqp, alloy->diff_pty);

  alloy->tk_bulk = NULL;
  alloy->ck_bulk = NULL;
  alloy->tx_bulk = NULL;
  alloy->cx_bulk = NULL;

  /* Physical constants */
  alloy->latent_heat = latent_heat;

  alloy->s_das = s_das;
  if (s_das < FLT_MIN)
    bft_error(__FILE__, __LINE__, 0,
              " %s: Invalid value %g for the secondary dendrite arms spacing",
              __func__, s_das);

  solid->forcing_coef = 180./(s_das*s_das);

  /* Phase diagram parameters */
  alloy->kp = kp;
  alloy->ml = mliq;
  alloy->t_eut = t_eutec;
  alloy->t_melt = t_melt;

  /* Derived parameters for the phase diagram */
  alloy->inv_kp = 1./kp;
  alloy->inv_kpm1 = 1./(alloy->kp - 1.);
  alloy->inv_ml = 1./mliq;
  alloy->c_eut = (t_eutec - t_melt)*alloy->inv_ml;
  alloy->cs1 = alloy->c_eut * kp; /* Apply the lever rule */
  alloy->dgldC_eut = 1./(alloy->c_eut - alloy->cs1);

  /* Define a small range of temperature around the eutectic temperature
   * in which one assumes an eutectic transformation */
  alloy->t_eut_inf =
    alloy->t_eut - cs_solidification_eutectic_threshold;
  alloy->t_eut_sup =
    alloy->t_eut + cs_solidification_eutectic_threshold;

  /* Numerical parameters (default values) and set function pointers
     accordingly to update properties/variables */
  alloy->iter = 0;
  alloy->n_iter_max = 5;
  alloy->delta_tolerance = 1e-3;
  alloy->gliq_relax = 0.;
  alloy->eta_relax = 0.;

  /* Default strategy: Legacy improvement with some Taylor expansions */
  alloy->strategy = CS_SOLIDIFICATION_STRATEGY_TAYLOR;

  /* Functions which are common to all strategies */
  alloy->thermosolutal_coupling = _default_binary_coupling;
  alloy->update_velocity_forcing = _update_velocity_forcing;
  alloy->update_clc = _update_clc;

  /* Functions which are specific to a strategy */
  alloy->update_gl = _update_gl_taylor;
  alloy->update_thm_st = _update_thm_taylor;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the main numerical parameters which described a solidification
 *         process with a binary alloy (with component A and B)
 *
 * \param[in]  strategy     strategy to perform the numerical segregation
 * \param[in]  n_iter_max   max.number of iterations for the C/T equations
 * \param[in]  tolerance    tolerance under which non-linear iter. stop
 * \param[in]  gliq_relax   relaxation coefficient for the update of the
 *                          liquid fraction
 * \param[in]  eta_relax    relaxation coefficient for the update of the
 *                          eta coefficient (scaling in front of the advective
 *                          term)
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_segregation_opt(cs_solidification_strategy_t  strategy,
                                      int                           n_iter_max,
                                      double                        tolerance,
                                      double                        gliq_relax,
                                      double                        eta_relax)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  /* Sanity checks */
  assert(n_iter_max > 0 && tolerance > 0);
  assert(solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY);
  assert(alloy != NULL);

  /* Numerical parameters */
  alloy->n_iter_max = n_iter_max;
  alloy->delta_tolerance = tolerance;

  alloy->gliq_relax = gliq_relax;
  alloy->eta_relax = eta_relax;

  alloy->strategy = strategy;
  if (strategy == CS_SOLIDIFICATION_STRATEGY_LEGACY) {

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM)
      alloy->update_gl = _update_gl_legacy_ast;
    else
      alloy->update_gl = _update_gl_legacy;

    alloy->update_thm_st = _update_thm_legacy;

  }
  else if (strategy == CS_SOLIDIFICATION_STRATEGY_TAYLOR) {

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM)
      bft_error(__FILE__, __LINE__, 0,
                "%s: Adding an advective source term is incompatible with"
                " the Taylor strategy.\n", __func__);
    else
      alloy->update_gl = _update_gl_taylor;

    alloy->update_thm_st = _update_thm_taylor;

  }
  else if (strategy == CS_SOLIDIFICATION_STRATEGY_PATH) {

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM)
      bft_error(__FILE__, __LINE__, 0,
                "%s: Adding an advective source term is incompatible with"
                " the Path strategy.\n", __func__);
    else
      alloy->update_gl = _update_gl_path;

    alloy->update_thm_st = _update_thm_path;

  }

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set the functions to perform the update of physical properties
 *         and/or the computation of the thermal source term or quantities
 *         and/or the way to perform the coupling between the thermal equation
 *         and the bulk concentration computation. All this setting defines
 *         the way to compute the solidification process of a binary alloy.
 *         If a function is set to NULL then the automatic settings is kept.
 *
 *         --Advanced usage-- This enables to finely control the numerical or
 *         physical modelling aspects.
 *
 * \param[in] vel_forcing        pointer to update the velocity forcing
 * \param[in] cliq_update        pointer to update the liquid concentration
 * \param[in] gliq_update        pointer to update the liquid fraction
 * \param[in] thm_st_update      pointer to update thermal source terms
 * \param[in] thm_conc_coupling  pointer to compute the thermo-solutal coupling
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_set_functions(cs_solidification_func_t  *vel_forcing,
                                cs_solidification_func_t  *cliq_update,
                                cs_solidification_func_t  *gliq_update,
                                cs_solidification_func_t  *thm_st_update,
                                cs_solidification_func_t  *thm_conc_coupling)
{
  cs_solidification_t  *solid = cs_solidification_structure;
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  cs_solidification_binary_alloy_t  *alloy
    = (cs_solidification_binary_alloy_t *)solid->model_context;

  assert(solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY);
  assert(alloy != NULL);

  if (vel_forcing != NULL) {
    alloy->update_velocity_forcing = vel_forcing;
    solid->options |= CS_SOLIDIFICATION_BINARY_ALLOY_M_FUNC;
  }

  if (cliq_update != NULL) {
    alloy->update_clc = cliq_update;
    solid->options |= CS_SOLIDIFICATION_BINARY_ALLOY_C_FUNC;
  }

  if (gliq_update != NULL) {
    alloy->update_gl = gliq_update;
    solid->options |= CS_SOLIDIFICATION_BINARY_ALLOY_G_FUNC;
  }

  if (thm_st_update != NULL) {
    alloy->update_thm_st = thm_st_update;
    solid->options |= CS_SOLIDIFICATION_BINARY_ALLOY_T_FUNC;
  }

  if (thm_conc_coupling != NULL) {
    alloy->thermosolutal_coupling = thm_conc_coupling;
    solid->options |= CS_SOLIDIFICATION_BINARY_ALLOY_TCC_FUNC;
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Free the main structure related to the solidification module
 *
 * \return a NULL pointer
 */
/*----------------------------------------------------------------------------*/

cs_solidification_t *
cs_solidification_destroy_all(void)
{
  if (cs_solidification_structure == NULL)
    return NULL;

  cs_solidification_t  *solid = cs_solidification_structure;

  /* The lifecycle of properties, equations and fields is not managed by
   * the current structure and sub-structures.
   * Free only what is owned by this structure */

  if (solid->model & CS_SOLIDIFICATION_MODEL_VOLLER_PRAKASH_87) {

    cs_solidification_voller_t  *v_model
      = (cs_solidification_voller_t *)solid->model_context;

    BFT_FREE(v_model);

  } /* Voller and Prakash modelling */

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    BFT_FREE(alloy->diff_pty_array);
    BFT_FREE(alloy->c_l_cells);
    BFT_FREE(alloy->eta_coef_array);
    BFT_FREE(alloy->tk_bulk);
    BFT_FREE(alloy->ck_bulk);

    if (solid->options & CS_SOLIDIFICATION_USE_EXTRAPOLATION) {
      BFT_FREE(alloy->tx_bulk);
      BFT_FREE(alloy->cx_bulk);
    }

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM)
      BFT_FREE(alloy->c_l_faces);

    if (solid->post_flag & CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE)
      BFT_FREE(alloy->t_liquidus);

    if (solid->post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS) {
      BFT_FREE(alloy->tbulk_minus_tliq);
      BFT_FREE(alloy->cliq_minus_cbulk);
    }

    BFT_FREE(alloy);

  } /* Binary alloy modelling */

  BFT_FREE(solid->thermal_reaction_coef_array);
  BFT_FREE(solid->thermal_source_term_array);
  BFT_FREE(solid->forcing_mom_array);

  BFT_FREE(solid->cell_state);

  if (solid->plot_state != NULL)
    cs_time_plot_finalize(&solid->plot_state);

  BFT_FREE(solid);

  return NULL;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Setup equations/properties related to the Solidification module
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_init_setup(void)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  const int  field_mask = CS_FIELD_INTENSIVE | CS_FIELD_CDO;
  const int  log_key = cs_field_key_id("log");
  const int  post_key = cs_field_key_id("post_vis");
  const int  c_loc_id = cs_mesh_location_get_id_by_name("cells");

  /* Add a field for the liquid fraction */
  solid->g_l_field = cs_field_create("liquid_fraction",
                                     field_mask,
                                     c_loc_id,
                                     1,
                                     true); /* has_previous */

  cs_field_set_key_int(solid->g_l_field, log_key, 1);
  cs_field_set_key_int(solid->g_l_field, post_key, 1);

  /* Add a reaction term to the momentum equation */
  cs_equation_t  *mom_eq = cs_navsto_system_get_momentum_eq();
  cs_equation_param_t  *mom_eqp = cs_equation_get_param(mom_eq);
  assert(mom_eqp != NULL);

  cs_equation_add_reaction(mom_eqp, solid->forcing_mom);

  /* Add default post-processing related to the solidifcation module */
  cs_post_add_time_mesh_dep_output(cs_solidification_extra_post, solid);

  /* Model-specific part */

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    cs_equation_param_t  *eqp = cs_equation_get_param(alloy->solute_equation);

    /* Add the unsteady term */
    cs_equation_add_time(eqp, solid->mass_density);

    /* Add an advection term to the solute concentration equation */
    cs_equation_add_advection(eqp, cs_navsto_get_adv_field());

    if ((solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM)
        == 0) {

      alloy->eta_coef_pty = cs_property_add("alloy_adv_coef", CS_PROPERTY_ISO);

      cs_equation_add_advection_scaling_property(eqp, alloy->eta_coef_pty);

    }

  } /* Binary alloy model */

  if (cs_glob_rank_id < 1) {

    int  n_output_states = CS_SOLIDIFICATION_N_STATES - 1;
    if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY)
      n_output_states += 1;

    int  n_output_values = n_output_states;
    if (solid->post_flag & CS_SOLIDIFICATION_POST_SOLIDIFICATION_RATE)
      n_output_values += 1;

    if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {
      if (solid->post_flag & CS_SOLIDIFICATION_POST_SEGREGATION_INDEX)
        n_output_values += 1;
    }

    const char  **labels;
    BFT_MALLOC(labels, n_output_values, const char *);
    for (int i = 0; i < n_output_states; i++)
      labels[i] = _state_names[i];

    n_output_values = n_output_states;
    if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY)
      if (solid->post_flag & CS_SOLIDIFICATION_POST_SEGREGATION_INDEX)
        labels[n_output_values++] = "SegrIndex";

    if (solid->post_flag & CS_SOLIDIFICATION_POST_SOLIDIFICATION_RATE)
      labels[n_output_values++] = "SolidRate";

    /* Use the physical time rather than the number of iterations */
    solid->plot_state = cs_time_plot_init_probe("solidification",
                                                "",
                                                CS_TIME_PLOT_DAT,
                                                false,
                                                180,   /* flush time */
                                                -1,
                                                n_output_values,
                                                NULL,
                                                NULL,
                                                labels);

    BFT_FREE(labels);

  } /* rank 0 */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Finalize the setup stage for equations related to the solidification
 *         module
 *
 * \param[in]  connect    pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_finalize_setup(const cs_cdo_connect_t       *connect,
                                 const cs_cdo_quantities_t    *quant)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  const cs_lnum_t  n_cells = quant->n_cells;
  const size_t  size_c = n_cells*sizeof(cs_real_t);

  /* Retrieve the field associated to the temperature */
  solid->temperature = cs_field_by_name("temperature");

  /* Define the liquid fraction */
  cs_property_def_by_field(solid->g_l, solid->g_l_field);

  /* Initially one assumes that all is liquid except for cells in a
   * predefined sold zone for all the computation */
  BFT_MALLOC(solid->cell_state, n_cells, cs_solidification_state_t);

  cs_field_set_values(solid->g_l_field, 1.);

# pragma omp parallel for if (n_cells > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_cells; i++) {

    if (connect->cell_flag[i] & CS_FLAG_SOLID_CELL) {
      solid->g_l_field->val[i] = 0;
      solid->g_l_field->val_pre[i] = 0;
      solid->cell_state[i] = CS_SOLIDIFICATION_STATE_SOLID;
    }
    else {
      solid->g_l_field->val_pre[i] = 1.;
      solid->cell_state[i] = CS_SOLIDIFICATION_STATE_LIQUID;

    }

  } /* Loop on cells */

  /* Add the boussinesq source term in the momentum equation */
  cs_equation_t  *mom_eq = cs_navsto_system_get_momentum_eq();
  assert(mom_eq != NULL);
  cs_equation_param_t  *mom_eqp = cs_equation_get_param(mom_eq);
  cs_physical_constants_t  *phy_constants = cs_get_glob_physical_constants();

  /* Define the metadata to build a Boussinesq source term related to the
   * temperature. This structure is allocated here but the lifecycle is
   * managed by the cs_thermal_system_t structure */
  cs_source_term_boussinesq_t  *thm_bq =
    cs_thermal_system_add_boussinesq_term(phy_constants->gravity,
                                          solid->mass_density->ref_value);

  cs_dof_func_t  *func = NULL;
  if (solid->model & CS_SOLIDIFICATION_MODEL_VOLLER_PRAKASH_87)
    func = _temp_boussinesq_source_term;
  else if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY)
    func = _temp_conc_boussinesq_source_term;
  else
    bft_error(__FILE__, __LINE__, 0,
              " %s: This model is not handled yet.", __func__);

  cs_equation_add_source_term_by_dof_func(mom_eqp,
                                          NULL, /* = all cells */
                                          cs_flag_primal_cell,
                                          func,
                                          thm_bq);

  /* Define the forcing term acting as a reaction term in the momentum equation
     This term is related to the liquid fraction */
  BFT_MALLOC(solid->forcing_mom_array, n_cells, cs_real_t);
  memset(solid->forcing_mom_array, 0, size_c);

  cs_property_def_by_array(solid->forcing_mom,
                           cs_flag_primal_cell,
                           solid->forcing_mom_array,
                           false, /* definition is owner ? */
                           NULL); /* no index */

  /* Define the reaction coefficient and the source term for the temperature
     equation */
  if (solid->thermal_reaction_coef != NULL) {

    BFT_MALLOC(solid->thermal_reaction_coef_array, n_cells, cs_real_t);
    memset(solid->thermal_reaction_coef_array, 0, size_c);

    cs_property_def_by_array(solid->thermal_reaction_coef,
                             cs_flag_primal_cell,
                             solid->thermal_reaction_coef_array,
                             false, /* definition is owner ? */
                             NULL); /* no index */

    BFT_MALLOC(solid->thermal_source_term_array, n_cells, cs_real_t);
    memset(solid->thermal_source_term_array, 0, size_c);

    cs_equation_param_t  *thm_eqp = cs_equation_param_by_name(CS_THERMAL_EQNAME);
    cs_equation_add_source_term_by_array(thm_eqp,
                                         NULL,   /* all cells selected */
                                         cs_flag_primal_cell,
                                         solid->thermal_source_term_array,
                                         false,  /* definition is owner ? */
                                         NULL);  /* no index */

  }

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {
    /*               ==================================== */

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    /* Get a shortcut to the c_bulk field */
    alloy->c_bulk = cs_equation_get_field(alloy->solute_equation);

    /* Allocate an array to store the liquid concentration and arrays storing
       the intermediate states during the sub-iterations to solve the
       non-linearity */
    BFT_MALLOC(alloy->c_l_cells, n_cells, cs_real_t);
    BFT_MALLOC(alloy->tk_bulk, n_cells, cs_real_t);
    BFT_MALLOC(alloy->ck_bulk, n_cells, cs_real_t);

    if (solid->options & CS_SOLIDIFICATION_USE_EXTRAPOLATION) {
      BFT_MALLOC(alloy->tx_bulk, n_cells, cs_real_t);
      BFT_MALLOC(alloy->cx_bulk, n_cells, cs_real_t);
    }

    /* Allocate eta even if SOLUTE_WITH_SOURCE_TERM is activated */
    const cs_real_t  eta_ref_value = 1.;
    BFT_MALLOC(alloy->eta_coef_array, n_cells, cs_real_t);
#   pragma omp parallel for if (n_cells > CS_THR_MIN)
    for (cs_lnum_t i = 0; i < n_cells; i++)
      alloy->eta_coef_array[i] = eta_ref_value;

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM) {

      BFT_MALLOC(alloy->c_l_faces, quant->n_faces, cs_real_t);
      memset(alloy->c_l_faces, 0, sizeof(cs_real_t)*quant->n_faces);

    }
    else { /* Estimate the reference value for the solutal diffusion property
            * One assumes that g_l (the liquid fraction is equal to 1) */

      cs_property_set_reference_value(alloy->eta_coef_pty, eta_ref_value);

      cs_property_def_by_array(alloy->eta_coef_pty,
                               cs_flag_primal_cell,
                               alloy->eta_coef_array,
                               false,
                               NULL);
    }

    /* Estimate the reference value for the solutal diffusion property
     * One assumes that g_l (the liquid fraction is equal to 1) */
    const cs_real_t  pty_ref_value =
      solid->mass_density->ref_value*alloy->diff_coef;

    cs_property_set_reference_value(alloy->diff_pty, pty_ref_value);

    BFT_MALLOC(alloy->diff_pty_array, n_cells, cs_real_t);

#   pragma omp parallel for if (n_cells > CS_THR_MIN)
    for (cs_lnum_t i = 0; i < n_cells; i++)
      alloy->diff_pty_array[i] = pty_ref_value;

    cs_property_def_by_array(alloy->diff_pty,
                             cs_flag_primal_cell,
                             alloy->diff_pty_array,
                             false,
                             NULL);

    if (solid->post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS) {

      BFT_MALLOC(alloy->tbulk_minus_tliq, n_cells, cs_real_t);
      memset(alloy->tbulk_minus_tliq, 0, size_c);
      BFT_MALLOC(alloy->cliq_minus_cbulk, n_cells, cs_real_t);
      memset(alloy->cliq_minus_cbulk, 0, size_c);

    }

    if (solid->post_flag & CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE)
      BFT_MALLOC(alloy->t_liquidus, n_cells, cs_real_t);

  } /* Binary alloy model */

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Summarize the solidification module in the log file dedicated to
 *         the setup
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_log_setup(void)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  if (solid == NULL)
    return;

  cs_log_printf(CS_LOG_SETUP, "\nSummary of the solidification module\n");
  cs_log_printf(CS_LOG_SETUP, "%s\n", cs_sep_h1);

  cs_log_printf(CS_LOG_SETUP, "  * Solidification | Verbosity: %d\n",
                solid->verbosity);

  cs_log_printf(CS_LOG_SETUP, "  * Solidification | Model:");
  if (cs_flag_test(solid->model, CS_SOLIDIFICATION_MODEL_VOLLER_PRAKASH_87)) {

    cs_solidification_voller_t  *v_model
      = (cs_solidification_voller_t *)solid->model_context;

    cs_log_printf(CS_LOG_SETUP, "Voller-Prakash (1987)\n");
    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Tliq: %5.3e; Tsol: %5.3e",
                  v_model->t_liquidus, v_model->t_solidus);
    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Latent heat: %5.3e\n",
                  v_model->latent_heat);
    cs_log_printf(CS_LOG_SETUP,
                  "  * Solidification | Forcing coef: %5.3e s_das: %5.3e\n",
                  solid->forcing_coef, v_model->s_das);

  }
  else if (cs_flag_test(solid->model, CS_SOLIDIFICATION_MODEL_BINARY_ALLOY)) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    cs_log_printf(CS_LOG_SETUP, "Binary alloy\n");
    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Alloy: %s\n",
                  cs_equation_get_name(alloy->solute_equation));

    cs_log_printf(CS_LOG_SETUP,
                  "  * Solidification | Dilatation coef. concentration: %5.3e\n"
                  "  * Solidification | Distribution coef.: %5.3e\n"
                  "  * Solidification | Liquidus slope: %5.3e\n"
                  "  * Solidification | Phase change temp.: %5.3e\n"
                  "  * Solidification | Eutectic conc.: %5.3e\n"
                  "  * Solidification | Reference concentration: %5.3e\n"
                  "  * Solidification | Latent heat: %5.3e\n",
                  alloy->dilatation_coef, alloy->kp, alloy->ml, alloy->t_melt,
                  alloy->c_eut, alloy->ref_concentration, alloy->latent_heat);
    cs_log_printf(CS_LOG_SETUP,
                  "  * Solidification | Forcing coef: %5.3e; s_das: %5.3e\n",
                  solid->forcing_coef, alloy->s_das);

    /* Display options */
    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Strategy:");
    switch (alloy->strategy) {

    case CS_SOLIDIFICATION_STRATEGY_LEGACY:
      cs_log_printf(CS_LOG_SETUP, " Legacy\n");
      break;
    case CS_SOLIDIFICATION_STRATEGY_TAYLOR:
      cs_log_printf(CS_LOG_SETUP, " Legacy + Taylor-based updates\n");
      break;
    case CS_SOLIDIFICATION_STRATEGY_PATH:
      cs_log_printf(CS_LOG_SETUP, " Rely on the solidification path\n");
      break;

    default:
      bft_error(__FILE__, __LINE__, 0, "%s: Invalid strategy\n", __func__);
    }

    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Options:");
    if (solid->options & CS_SOLIDIFICATION_BINARY_ALLOY_C_FUNC)
      cs_log_printf(CS_LOG_SETUP,
                    " User-defined function for the concentration eq.");
    else {

      if (cs_flag_test(solid->options,
                       CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM))
        cs_log_printf(CS_LOG_SETUP,
                      " Solute concentration with an advective source term");
      else
        cs_log_printf(CS_LOG_SETUP,
                      " Solute concentration with an advective coefficient");

    } /* Not user-defined */
    cs_log_printf(CS_LOG_SETUP, "\n");

    if (solid->options & CS_SOLIDIFICATION_BINARY_ALLOY_T_FUNC)
      cs_log_printf(CS_LOG_SETUP,
                    "  * Solidification | Options:"
                    " User-defined function for the thermal eq.\n");

    if (solid->options & CS_SOLIDIFICATION_BINARY_ALLOY_G_FUNC)
      cs_log_printf(CS_LOG_SETUP,
                    "  * Solidification | Options:"
                    " User-defined function for the liquid fraction/state\n");

    cs_log_printf(CS_LOG_SETUP, "  * Solidification | Options:");
    if (solid->options & CS_SOLIDIFICATION_BINARY_ALLOY_TCC_FUNC)
      cs_log_printf(CS_LOG_SETUP,
                    " User-defined function for the thermo-solutal coupling");
    else
      cs_log_printf(CS_LOG_SETUP,
                    " Default thermo-solutal coupling algorithm");
    cs_log_printf(CS_LOG_SETUP, "\n");

    if (cs_flag_test(solid->options,
                     CS_SOLIDIFICATION_USE_EXTRAPOLATION))
      cs_log_printf(CS_LOG_SETUP,
                    "  * Solidification | Options:"
                    " Update using a second-order in time extrapolation\n");

    if (solid->options & CS_SOLIDIFICATION_WITH_PENALIZED_EUTECTIC) {
      if (alloy->strategy == CS_SOLIDIFICATION_STRATEGY_PATH)
        cs_log_printf(CS_LOG_SETUP,
                      "  * Solidification | Options:"
                      " Penalized eutectic temperature\n");
      else
        cs_log_printf(CS_LOG_SETUP,
                      "  * Solidification | Options:"
                      " Penalized eutectic temperature (unused)\n");
    }

    if (alloy->n_iter_max > 1)
      cs_log_printf(CS_LOG_SETUP,
                    "  * Solidification | Options:"
                    " Sub-iterations requested with "
                    " n_iter_max %d; tolerance: %.3e\n",
                    alloy->n_iter_max, alloy->delta_tolerance);

  } /* Binary alloy */

  cs_log_printf(CS_LOG_SETUP, "\n");

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Initialize the context structure used to build the algebraic system
 *         This is done after the setup step.
 *
 * \param[in]      mesh       pointer to a cs_mesh_t structure
 * \param[in]      connect    pointer to a cs_cdo_connect_t structure
 * \param[in]      quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]      time_step  pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_initialize(const cs_mesh_t              *mesh,
                             const cs_cdo_connect_t       *connect,
                             const cs_cdo_quantities_t    *quant,
                             const cs_time_step_t         *time_step)
{
  CS_UNUSED(mesh);
  CS_UNUSED(connect);
  CS_UNUSED(time_step);

  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  /* Set the first fluid/solid cell and sanity check for the mass density in the
     fluid/solid zone */
  const cs_property_t  *cp_p = solid->thermal_sys->cp;

  for (int i = 0; i < cs_volume_zone_n_zones(); i++) {

    const cs_zone_t  *z = cs_volume_zone_by_id(i);

    if (z->type & CS_VOLUME_ZONE_SOLID) /* permanent solid zone */
      continue;

    else { /* fluid/solid zone according to thermodynamics conditions */

      if (z->n_elts == 0)
        continue;

      if (solid->first_cell < 0) {
        solid->first_cell = z->elt_ids[0];
        solid->rho0 = cs_property_get_cell_value(solid->first_cell,
                                                 time_step->t_cur,
                                                 solid->mass_density);
        solid->cp0 = cs_property_get_cell_value(solid->first_cell,
                                                time_step->t_cur,
                                                cp_p);

      }
      else {

        cs_real_t  rho = cs_property_get_cell_value(solid->first_cell,
                                                    time_step->t_cur,
                                                    solid->mass_density);
        if (fabs(rho - solid->rho0) > FLT_MIN)
          bft_error(__FILE__, __LINE__, 0,
                    "%s: A uniform value of the mass density in the"
                    " solidification/melting area is assumed.\n"
                    " Please check your settings.\n"
                    " rho0= %5.3e and rho= %5.3e in zone %s\n",
                    __func__, solid->rho0, rho, z->name);

        cs_real_t  cp = cs_property_get_cell_value(solid->first_cell,
                                                   time_step->t_cur,
                                                   cp_p);

        if (fabs(cp - solid->cp0) > FLT_MIN)
          bft_error(__FILE__, __LINE__, 0,
                    "%s: A uniform value of the Cp property in the"
                    " solidification/melting area is assumed.\n"
                    " Please check your settings.\n"
                    " cp0= %5.3e and cp= %5.3e in zone %s\n",
                    __func__, solid->cp0, cp, z->name);

      }

    } /* solidification/melting zone */

  } /* Loop on volume zones */

  if (fabs(solid->rho0 - solid->mass_density->ref_value) > FLT_MIN) {
    cs_base_warn(__FILE__, __LINE__);
    bft_printf(" %s: Reference value of the mass density seems not unique.\n"
               " solid->rho0: %5.3e; mass_density->ref_value: %5.3e\n"
               " Please check your settings.", __func__,
               solid->rho0, solid->mass_density->ref_value);
    printf(" %s >> Warning >> reference value for the mass density\n",
           __func__);
  }

  /* End of sanity checks */
  /* -------------------- */

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    if (solid->options & CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM) {

      if (cs_equation_get_space_scheme(alloy->solute_equation) !=
          CS_SPACE_SCHEME_CDOFB)
        bft_error(__FILE__, __LINE__, 0,
                  " %s: Invalid space scheme for equation %s\n",
                  __func__, cs_equation_get_name(alloy->solute_equation));

      cs_equation_add_user_hook(alloy->solute_equation,
                                NULL,                    /* hook context */
                                _fb_solute_source_term); /* hook function */

      /* Store the pointer to the current face temperature values */
      alloy->temp_faces =
        cs_equation_get_face_values(solid->thermal_sys->thermal_eq, false);

    } /* CS_SOLIDIFICATION_WITH_SOLUTE_SOURCE_TERM */

    /* One assumes that all the alloy mixture is liquid thus C_l = C_bulk */
    const cs_lnum_t  n_cells = quant->n_cells;
    memcpy(alloy->c_l_cells, alloy->c_bulk->val, n_cells*sizeof(cs_real_t));

    /* Set the previous iterate before calling update functions */
    memcpy(alloy->tk_bulk, solid->temperature->val, n_cells*sizeof(cs_real_t));
    memcpy(alloy->ck_bulk, alloy->c_bulk->val, n_cells*sizeof(cs_real_t));

    if (alloy->c_l_faces != NULL) {
      cs_real_t  *c_bulk_faces =
        cs_equation_get_face_values(alloy->solute_equation, false);
      memcpy(alloy->c_l_faces, c_bulk_faces, quant->n_faces*sizeof(cs_real_t));
    }

  } /* CS_SOLIDIFICATION_MODEL_BINARY_ALLOY */

  else {

    cs_solidification_voller_t  *v_model
      = (cs_solidification_voller_t *)solid->model_context;

    v_model->update(mesh, connect, quant, time_step);

  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Solve equations related to the solidification module
 *
 * \param[in]      mesh       pointer to a cs_mesh_t structure
 * \param[in]      time_step  pointer to a cs_time_step_t structure
 * \param[in]      connect    pointer to a cs_cdo_connect_t structure
 * \param[in]      quant      pointer to a cs_cdo_quantities_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_compute(const cs_mesh_t              *mesh,
                          const cs_time_step_t         *time_step,
                          const cs_cdo_connect_t       *connect,
                          const cs_cdo_quantities_t    *quant)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  /* Sanity checks */
  if (solid == NULL) bft_error(__FILE__, __LINE__, 0, _(_err_empty_module));

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;

    alloy->thermosolutal_coupling(mesh, connect, quant, time_step);

  }
  else { /* Solidification process with a pure component without segregation */

    cs_solidification_voller_t  *v_model
      = (cs_solidification_voller_t *)solid->model_context;

    /* Add equations to be solved at each time step */
    cs_thermal_system_compute(true, /* operate a cur2prev operation inside */
                              mesh, time_step, connect, quant);

    /* Update fields and properties which are related to solved variables */
    cs_field_current_to_previous(solid->g_l_field);

    v_model->update(mesh, connect, quant, time_step);

  }

  /* Solve the Navier-Stokes system */
  cs_navsto_system_compute(mesh, time_step, connect, quant);

  /* Perform the monitoring */
  if (solid->verbosity > 0)
    _do_monitoring(quant);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Predefined extra-operations for the solidification module
 *
 * \param[in]  connect   pointer to a cs_cdo_connect_t structure
 * \param[in]  quant      pointer to a cs_cdo_quantities_t structure
 * \param[in]  ts         pointer to a cs_time_step_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_extra_op(const cs_cdo_connect_t      *connect,
                           const cs_cdo_quantities_t   *quant,
                           const cs_time_step_t        *ts)
{
  cs_solidification_t  *solid = cs_solidification_structure;

  if (solid == NULL)
    return;

  /* Estimate the number of values to output */
  int  n_output_values = CS_SOLIDIFICATION_N_STATES - 1;
  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {
    n_output_values += 1;

    if (solid->post_flag & CS_SOLIDIFICATION_POST_SEGREGATION_INDEX)
      n_output_values += 1;

  }

  if (solid->post_flag & CS_SOLIDIFICATION_POST_SOLIDIFICATION_RATE)
    n_output_values += 1;

  /* Compute the output values */
  cs_real_t  *output_values = NULL;
  BFT_MALLOC(output_values, n_output_values, cs_real_t);
  memset(output_values, 0, n_output_values*sizeof(cs_real_t));

  int n_output_states = (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) ?
    CS_SOLIDIFICATION_N_STATES : CS_SOLIDIFICATION_N_STATES - 1;
  for (int i = 0; i < n_output_states; i++)
    output_values[i] = solid->state_ratio[i];

  n_output_values = n_output_states;

  if (solid->post_flag & CS_SOLIDIFICATION_POST_SOLIDIFICATION_RATE) {

    const cs_real_t  *gl = solid->g_l_field->val;

    cs_real_t  integr = 0;
    for (cs_lnum_t i = 0; i < quant->n_cells; i++) {
      if (connect->cell_flag[i] & CS_FLAG_SOLID_CELL)
        continue;
      integr += (1 - gl[i])*quant->cell_vol[i];
    }

    /* Parallel reduction */
    cs_parall_sum(1, CS_REAL_TYPE, &integr);

    output_values[n_output_values] = integr/quant->vol_tot;
    n_output_values++;

  }

  if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

    cs_solidification_binary_alloy_t  *alloy
      = (cs_solidification_binary_alloy_t *)solid->model_context;
    assert(alloy != NULL);

    const cs_real_t  *c_bulk = alloy->c_bulk->val;

    if (solid->post_flag & CS_SOLIDIFICATION_POST_SEGREGATION_INDEX) {

      const cs_real_t  inv_cref = 1./alloy->ref_concentration;

      cs_real_t  si = 0;
      for (cs_lnum_t i = 0; i < quant->n_cells; i++) {
        if (connect->cell_flag[i] & CS_FLAG_SOLID_CELL)
          continue;
        double  c = (c_bulk[i] - alloy->ref_concentration)*inv_cref;
        si += c*c*quant->cell_vol[i];
      }

      /* Parallel reduction */
      cs_parall_sum(1, CS_REAL_TYPE, &si);

      output_values[n_output_values] = sqrt(si/quant->vol_tot);
      n_output_values++;

    }

    if (solid->post_flag & CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE) {
      assert(alloy->t_liquidus != NULL);

      /* Compute the value to be sure that it corresponds to the current state */
      for (cs_lnum_t i = 0; i < quant->n_cells; i++) {
        if (connect->cell_flag[i] & CS_FLAG_SOLID_CELL)
          alloy->t_liquidus[i] = -999.99; /* no physical meaning */
        else
          alloy->t_liquidus[i] = _get_t_liquidus(alloy, alloy->c_bulk->val[i]);
      }

    }

    if ((solid->post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS) > 0) {

      assert(alloy->t_liquidus != NULL &&
             alloy->cliq_minus_cbulk != NULL &&
             alloy->tbulk_minus_tliq != NULL);

      const cs_real_t  *c_l = alloy->c_l_cells;
      const cs_real_t  *t_bulk = solid->temperature->val;

      /* Compute Cbulk - Cliq */
      for (cs_lnum_t c_id = 0; c_id < quant->n_cells; c_id++) {

        if (connect->cell_flag[c_id] & CS_FLAG_SOLID_CELL)
          continue; /* = 0 by default */

        const cs_real_t  conc = c_bulk[c_id];
        const cs_real_t  temp = t_bulk[c_id];

        alloy->cliq_minus_cbulk[c_id] = c_l[c_id] - conc;
        alloy->tbulk_minus_tliq[c_id] = temp - alloy->t_liquidus[c_id];

      } /* Loop on cells */

    } /* Advanced analysis */

  } /* Binary alloy modelling */

  if (cs_glob_rank_id < 1 && solid->plot_state != NULL)
    cs_time_plot_vals_write(solid->plot_state,
                            ts->nt_cur,
                            ts->t_cur,
                            n_output_values,
                            output_values);

  BFT_FREE(output_values);

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Predefined post-processing output for the solidification module.
 *         Prototype of this function is fixed since it is a function pointer
 *         defined in cs_post.h (\ref cs_post_time_mesh_dep_output_t)
 *
 * \param[in, out] input        pointer to a optional structure (here a
 *                              cs_gwf_t structure)
 * \param[in]      mesh_id      id of the output mesh for the current call
 * \param[in]      cat_id       category id of the output mesh for this call
 * \param[in]      ent_flag     indicate global presence of cells (ent_flag[0]),
 *                              interior faces (ent_flag[1]), boundary faces
 *                              (ent_flag[2]), particles (ent_flag[3]) or probes
 *                              (ent_flag[4])
 * \param[in]      n_cells      local number of cells of post_mesh
 * \param[in]      n_i_faces    local number of interior faces of post_mesh
 * \param[in]      n_b_faces    local number of boundary faces of post_mesh
 * \param[in]      cell_ids     list of cells (0 to n-1)
 * \param[in]      i_face_ids   list of interior faces (0 to n-1)
 * \param[in]      b_face_ids   list of boundary faces (0 to n-1)
 * \param[in]      time_step    pointer to a cs_time_step_t struct.
 */
/*----------------------------------------------------------------------------*/

void
cs_solidification_extra_post(void                      *input,
                             int                        mesh_id,
                             int                        cat_id,
                             int                        ent_flag[5],
                             cs_lnum_t                  n_cells,
                             cs_lnum_t                  n_i_faces,
                             cs_lnum_t                  n_b_faces,
                             const cs_lnum_t            cell_ids[],
                             const cs_lnum_t            i_face_ids[],
                             const cs_lnum_t            b_face_ids[],
                             const cs_time_step_t      *time_step)
{
  CS_UNUSED(n_i_faces);
  CS_UNUSED(n_b_faces);
  CS_UNUSED(cell_ids);
  CS_UNUSED(i_face_ids);
  CS_UNUSED(b_face_ids);

  if (input == NULL)
    return;

  cs_solidification_t  *solid = (cs_solidification_t *)input;

  if (cat_id == CS_POST_MESH_PROBES) {

    cs_field_t  *fld = cs_field_by_name_try("liquid_fraction");
    assert(fld != NULL);

    cs_post_write_probe_values(mesh_id,
                               CS_POST_WRITER_ALL_ASSOCIATED,
                               "liquid_fraction",
                               fld->dim,
                               CS_POST_TYPE_cs_real_t,
                               CS_MESH_LOCATION_CELLS,
                               NULL,
                               NULL,
                               fld->val,
                               time_step);

    if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

      cs_solidification_binary_alloy_t  *alloy
        = (cs_solidification_binary_alloy_t *)solid->model_context;

      cs_post_write_probe_values(mesh_id,
                                 CS_POST_WRITER_ALL_ASSOCIATED,
                                 "C_l",
                                 1,
                                 CS_POST_TYPE_cs_real_t,
                                 CS_MESH_LOCATION_CELLS,
                                 NULL,
                                 NULL,
                                 alloy->c_l_cells,
                                 time_step);

      if (solid->post_flag & CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE) {
        assert(alloy->t_liquidus != NULL);
        cs_post_write_probe_values(mesh_id,
                                   CS_POST_WRITER_ALL_ASSOCIATED,
                                   "Tliquidus",
                                   1,
                                   CS_POST_TYPE_cs_real_t,
                                   CS_MESH_LOCATION_CELLS,
                                   NULL,
                                   NULL,
                                   alloy->t_liquidus,
                                   time_step);
      }

      if (solid->post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS) {

        cs_post_write_probe_values(mesh_id,
                                   CS_POST_WRITER_ALL_ASSOCIATED,
                                   "delta_cliq_minus_cbulk",
                                   1,
                                   CS_POST_TYPE_cs_real_t,
                                   CS_MESH_LOCATION_CELLS,
                                   NULL,
                                   NULL,
                                   alloy->cliq_minus_cbulk,
                                   time_step);

        cs_post_write_probe_values(mesh_id,
                                   CS_POST_WRITER_ALL_ASSOCIATED,
                                   "delta_tbulk_minus_tliq",
                                   1,
                                   CS_POST_TYPE_cs_real_t,
                                   CS_MESH_LOCATION_CELLS,
                                   NULL,
                                   NULL,
                                   alloy->tbulk_minus_tliq,
                                   time_step);

        if (alloy->eta_coef_array != NULL)
          cs_post_write_probe_values(mesh_id,
                                     CS_POST_WRITER_ALL_ASSOCIATED,
                                     "Cbulk_advection_scaling",
                                     1,
                                     CS_POST_TYPE_cs_real_t,
                                     CS_MESH_LOCATION_CELLS,
                                     NULL,
                                     NULL,
                                     alloy->eta_coef_array,
                                     time_step);

      } /* Advanced analysis */

    } /* Binary alloy model */

  } /* Probes */

  if ((cat_id == CS_POST_MESH_VOLUME) &&
      (ent_flag[0] == 1)) {     /* ent_flag == 1 --> on cells */

    if (solid->cell_state != NULL &&
        (solid->post_flag & CS_SOLIDIFICATION_POST_CELL_STATE)) {

      cs_post_write_var(CS_POST_MESH_VOLUME,
                        CS_POST_WRITER_DEFAULT,
                        "cell_state",
                        1,
                        false,  /* interlace */
                        true,   /* true = original mesh */
                        CS_POST_TYPE_int,
                        solid->cell_state, NULL, NULL,
                        time_step);

    }

    if (solid->model & CS_SOLIDIFICATION_MODEL_BINARY_ALLOY) {

      cs_solidification_binary_alloy_t  *alloy
        = (cs_solidification_binary_alloy_t *)solid->model_context;

      cs_real_t  *wb = cs_equation_get_tmpbuf();

      if (solid->post_flag & CS_SOLIDIFICATION_ADVANCED_ANALYSIS) {

        if (alloy->cliq_minus_cbulk != NULL)
          cs_post_write_var(CS_POST_MESH_VOLUME,
                            CS_POST_WRITER_DEFAULT,
                            "delta_cliq_minus_cbulk",
                            1,
                            false,  /* interlace */
                            true,   /* true = original mesh */
                            CS_POST_TYPE_cs_real_t,
                            alloy->cliq_minus_cbulk, NULL, NULL,
                            time_step);

        if (alloy->tbulk_minus_tliq != NULL)
          cs_post_write_var(CS_POST_MESH_VOLUME,
                            CS_POST_WRITER_DEFAULT,
                            "delta_tbulk_minus_tliq",
                            1,
                            false,  /* interlace */
                            true,   /* true = original mesh */
                            CS_POST_TYPE_cs_real_t,
                            alloy->tbulk_minus_tliq, NULL, NULL,
                            time_step);

        if (alloy->eta_coef_array != NULL)
          cs_post_write_var(CS_POST_MESH_VOLUME,
                            CS_POST_WRITER_DEFAULT,
                            "Cbulk_advection_scaling",
                            1,
                            false,  /* interlace */
                            true,   /* true = original mesh */
                            CS_POST_TYPE_cs_real_t,
                            alloy->eta_coef_array, NULL, NULL,
                            time_step);

      } /* Advanced analysis */

      if (solid->post_flag & CS_SOLIDIFICATION_POST_LIQUIDUS_TEMPERATURE) {

        if (alloy->t_liquidus != NULL)
          cs_post_write_var(CS_POST_MESH_VOLUME,
                            CS_POST_WRITER_DEFAULT,
                            "T_liquidus",
                            1,
                            false,  /* interlace */
                            true,   /* true = original mesh */
                            CS_POST_TYPE_cs_real_t,
                            alloy->t_liquidus, NULL, NULL,
                            time_step);

      }

      if (solid->post_flag & CS_SOLIDIFICATION_POST_CBULK_ADIM) {

        const cs_real_t  inv_cref = 1./alloy->ref_concentration;
        const cs_real_t  *c_bulk = alloy->c_bulk->val;

        for (cs_lnum_t i = 0; i < n_cells; i++)
          wb[i] = (c_bulk[i] - alloy->ref_concentration)*inv_cref;

        cs_post_write_var(CS_POST_MESH_VOLUME,
                          CS_POST_WRITER_DEFAULT,
                          "C_bulk_adim",
                          1,
                          false,  /* interlace */
                          true,   /* true = original mesh */
                          CS_POST_TYPE_cs_real_t,
                          wb, NULL, NULL,
                          time_step);

      } /* CS_SOLIDIFICATION_POST_CBULK_ADIM */

      if (solid->post_flag & CS_SOLIDIFICATION_POST_CLIQ)
        cs_post_write_var(CS_POST_MESH_VOLUME,
                          CS_POST_WRITER_DEFAULT,
                          "C_l",
                          1,
                          false,  /* interlace */
                          true,   /* true = original mesh */
                          CS_POST_TYPE_cs_real_t,
                          alloy->c_l_cells, NULL, NULL,
                          time_step);

    } /* Binary alloy model */

  } /* VOLUME_MESH + on cells */
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
