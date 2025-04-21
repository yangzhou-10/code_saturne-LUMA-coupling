/*============================================================================
 * Interpolation using MEDCoupling Intersector.
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

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_error.h"
#include "bft_mem.h"
#include "bft_printf.h"

#include "cs_file.h"
#include "cs_mesh.h"
#include "cs_mesh_connect.h"
#include "cs_parall.h"
#include "cs_post.h"
#include "cs_prototypes.h"
#include "cs_rotation.h"
#include "cs_selector.h"
#include "cs_timer.h"

#include "fvm_writer.h"
#include "fvm_nodal.h"
#include "fvm_nodal_append.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_medcoupling_utils.hxx"
#include "cs_medcoupling_intersector.h"

/*----------------------------------------------------------------------------
 * MEDCOUPLING library headers
 *----------------------------------------------------------------------------*/

#if defined(HAVE_MEDCOUPLING) && defined(HAVE_MEDCOUPLING_LOADER)

#include <MEDCoupling_version.h>

#include <MEDFileMesh.hxx>
#include <MEDCouplingUMesh.hxx>

#include <MEDFileField1TS.hxx>
#include <MEDCouplingField.hxx>
#include <MEDCouplingFieldFloat.hxx>
#include <MEDCouplingFieldDouble.hxx>
#include <MEDFileFieldMultiTS.hxx>

#include <MEDCouplingRemapper.hxx>

#include <MEDLoader.hxx>

#include <MEDCouplingNormalizedUnstructuredMesh.txx>
#include "Interpolation3D.hxx"

using namespace MEDCoupling;
#endif

/*----------------------------------------------------------------------------
 *  Intersector structure
 *----------------------------------------------------------------------------*/

struct _cs_medcoupling_intersector_t {

  char                           *name;
  char                           *medfile_path;
  char                           *interp_method;

  cs_medcoupling_mesh_t          *local_mesh;

#if defined(HAVE_MEDCOUPLING) && defined(HAVE_MEDCOUPLING_LOADER)
  MEDCouplingUMesh               *source_mesh;
#else
  void                           *source_mesh;
#endif

  cs_coord_3_t                   *init_coords;      // Array of coordiates
                                                    // of the MED object

  cs_coord_3_t                   *boundary_coords;  // Array of boundary nodes
                                                    // of the med objects
  cs_coord_3_t                   *init_boundary_coords;

  cs_lnum_t                      n_b_vertices;

  fvm_nodal_t                    *ext_mesh;         // Associated external mesh

  int                             matrix_needs_update;
  cs_real_t                      *vol_intersect;

};

/*============================================================================
 * Private global variables
 *============================================================================*/

static int                             _n_intersects = 0;
static int                             _writer_id    = 0;
static cs_medcoupling_intersector_t  **_intersects   = NULL;

#if defined(HAVE_MEDCOUPLING) && defined(HAVE_MEDCOUPLING_LOADER)

/*----------------------------------------------------------------------------*/
/*!
 * \brief create a cs_medcoupling_intersector_t object
 *
 * \return pointer to the newly created object
 */
/*----------------------------------------------------------------------------*/

static cs_medcoupling_intersector_t *
_create_intersector(void)
{

  cs_medcoupling_intersector_t *mi = NULL;
  BFT_MALLOC(mi, 1, cs_medcoupling_intersector_t);

  mi->name                = NULL;
  mi->medfile_path        = NULL;
  mi->interp_method       = NULL;
  mi->local_mesh          = NULL;
  mi->source_mesh         = NULL;
  mi->boundary_coords     = NULL;
  mi->n_b_vertices        = -1;
  mi->init_coords         = NULL;
  mi->ext_mesh            = NULL;
  mi->vol_intersect       = NULL;
  mi->matrix_needs_update = 1;

  return mi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Initialize a cs_medcoupling_intersector with given parameters
 *
 * \param[in] mi               pointer to the cs_medcoupling_intersector_t struct
 * \param[in] name             name of the intersector
 * \param[in] medfile_path     path to the MED file
 * \param[in] interp_method    interpolation method (P0P0, P1P0, ..)
 * \param[in] select_criteria  selection criteria
 *
 */
/*----------------------------------------------------------------------------*/

void
_allocate_intersector(cs_medcoupling_intersector_t *mi,
                      const char                   *name,
                      const char                   *medfile_path,
                      const char                   *interp_method,
                      const char                   *select_criteria)
{
  BFT_MALLOC(mi->name, strlen(name)+1, char);
  strcpy(mi->name, name);

  BFT_MALLOC(mi->medfile_path, strlen(medfile_path)+1, char);
  strcpy(mi->medfile_path, medfile_path);

  BFT_MALLOC(mi->interp_method, strlen(interp_method)+1, char);
  strcpy(mi->interp_method, interp_method);

  mi->local_mesh = cs_medcoupling_mesh_create(name, select_criteria, 3);
  cs_medcoupling_mesh_copy_from_base(cs_glob_mesh, mi->local_mesh, 1);

  mi->matrix_needs_update = 1;

  MEDCoupling::MEDFileUMesh *mesh = MEDCoupling::MEDFileUMesh::New(medfile_path);
  mi->source_mesh = mesh->getMeshAtLevel(0);

  /* Copy med mesh coordinates at init */
  cs_lnum_t n_vtx = mesh->getNumberOfNodes();
  const cs_lnum_t dim = mesh->getMeshDimension();
  BFT_MALLOC(mi->init_coords, n_vtx, cs_coord_3_t);

  DataArrayDouble *med_coords = DataArrayDouble::New();
  med_coords = mi->source_mesh->getCoordinatesAndOwner();

  for (cs_lnum_t i = 0; i < n_vtx; i++)
    for (cs_lnum_t j = 0; j < dim; j++)
      mi->init_coords[i][j] = med_coords->getIJ(i,j);

  /* Copy med mesh boundary coordinates */
  MEDCouplingUMesh  *b_mesh = mi->source_mesh->buildBoundaryMesh(false);
  b_mesh->convertAllToPoly();

  DataArrayDouble *b_coords = DataArrayDouble::New();

  b_coords= b_mesh->getCoordinatesAndOwner();

  cs_lnum_t n_b_vtx = b_mesh->getNumberOfNodes();

  mi->n_b_vertices = n_b_vtx;
  BFT_MALLOC(mi->boundary_coords, n_b_vtx, cs_coord_3_t);
  BFT_MALLOC(mi->init_boundary_coords, n_b_vtx, cs_coord_3_t);

  for (cs_lnum_t i = 0; i < n_b_vtx; i++){
    for (cs_lnum_t j = 0; j < dim; j++){
      mi->boundary_coords[i][j] = b_coords->getIJ(i,j);
      mi->init_boundary_coords[i][j] = b_coords->getIJ(i,j);
    }
  }

  /* Generate FVM structure */
  DataArrayIdType *vtx_lst = DataArrayIdType::New();
  DataArrayIdType *vtx_idx = DataArrayIdType::New();

  vtx_lst = b_mesh->getNodalConnectivity();
  vtx_idx = b_mesh->getNodalConnectivityIndex();
  cs_lnum_t n_b_faces = vtx_idx->getNbOfElems()-1;
  cs_lnum_t n_elt_lst = vtx_lst->getNbOfElems()- n_b_faces;

  cs_lnum_t  *vertex_num   = NULL;
  cs_lnum_t  *vertex_idx   = NULL;
  cs_gnum_t  *vertex_gnum  = NULL;
  cs_gnum_t  *faces_gnum   = NULL;
  cs_lnum_t  elem = 0;
  cs_lnum_t _n_b_faces = 0;

  if (cs_glob_rank_id < 1)
    _n_b_faces = n_b_faces;

  BFT_MALLOC(vertex_idx  , _n_b_faces +1, cs_lnum_t);
  vertex_idx[0] = 0;

  if (cs_glob_rank_id < 1) {
    BFT_MALLOC(faces_gnum  , _n_b_faces   ,  cs_gnum_t);
    BFT_MALLOC(vertex_num  , n_elt_lst , cs_lnum_t);
    BFT_MALLOC(vertex_gnum , n_b_vtx , cs_gnum_t);

    for (cs_lnum_t i = 0; i < _n_b_faces ; i++){
      vertex_idx[i] = vtx_idx->getIJ(i,0) - i;
      cs_lnum_t s_id = vtx_idx->getIJ(i,0);
      cs_lnum_t e_id = vtx_idx->getIJ(i+1,0);
      for(cs_lnum_t v_id = s_id +1 ; v_id < e_id; v_id ++){
        vertex_num[elem]  = vtx_lst->getIJ(v_id,0) + 1;
        elem ++;
      }
    }
    vertex_idx[_n_b_faces] = vtx_idx->getIJ(_n_b_faces,0) - _n_b_faces;

    for (cs_lnum_t i = 0; i < n_b_vtx; i++)
      vertex_gnum[i] = i + 1;

    for (cs_lnum_t i = 0; i < n_b_faces; i++)
      faces_gnum[i] = i + 1;
  }

  fvm_nodal_t *ext_mesh = fvm_nodal_create(mi->name, 3);

  fvm_nodal_append_by_transfer(ext_mesh,
                               _n_b_faces,
                               FVM_FACE_POLY,
                               NULL,
                               NULL,
                               vertex_idx,
                               vertex_num,
                               NULL);

  if (cs_glob_rank_id < 1) {
    fvm_nodal_set_shared_vertices(ext_mesh,
                                  (const cs_coord_t *)mi->boundary_coords);
  }
  else {
    fvm_nodal_set_shared_vertices(ext_mesh, NULL);
  }

  fvm_nodal_init_io_num(ext_mesh, faces_gnum , 2);
  fvm_nodal_init_io_num(ext_mesh, vertex_gnum, 0);

  fvm_nodal_dump(ext_mesh);

  mi->ext_mesh = ext_mesh;

  if (cs_glob_rank_id < 1) {
    BFT_FREE(vertex_gnum);
    BFT_FREE(faces_gnum);
  }

  /* Allocate volume intersection array */
  BFT_MALLOC(mi->vol_intersect, cs_glob_mesh->n_cells, cs_real_t);
  for (cs_lnum_t e_id = 0; e_id < cs_glob_mesh->n_cells; e_id++)
    mi->vol_intersect[e_id] = 0.;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute a matrix/vector product to apply a transformation to a vector
 * \brief Results is stored in an output array.
 *
 * \param[in]  matrix matrix
 * \param[in]  vector vector
 * \param[out] res    resulting vector
 *
 */
/*----------------------------------------------------------------------------*/

inline static void
_transform_coord_from_init(cs_real_t matrix[3][4],
                           cs_real_t vector[3],
                           cs_real_t res[3])
{
 /* homogeneous coords */
  cs_real_t  c_a[4] = {vector[0], vector[1], vector[2], 1.};
  cs_real_t  c_b[3] = {0, 0, 0};

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 4; j++)
      c_b[i] += matrix[i][j]*c_a[j];
  }

  for (int i = 0; i < 3; i++)
    res[i] = c_b[i];
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute a matrix/vector product to apply a transformation to a vector.
 *
 * \param[in]  matrix matrix
 * \param[in]  vector vector
 *
 */
/*----------------------------------------------------------------------------*/

inline static void
_transform_coord(cs_real_t matrix[3][4],
                 cs_real_t vector[3])
{
  /* homogeneous coords */
  double  c_a[4] = {vector[0], vector[1], vector[2], 1.};
  double  c_b[3] = {0, 0, 0};

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 4; j++)
      c_b[i] += matrix[i][j]*c_a[j];
  }

  for (int i = 0; i < 3; i++)
    vector[i] = c_b[i];
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief   assign vertex coordinates to a medcoupling mesh structure
 *
 * \param[in] med_mesh  pointer to MEDCouplingUMesh to which we copy the
 *                      coordinates
 * \param[in] coords    pointer to the coordinates to assign to the
 *                      MEDCouplingUMesh
 */
/*----------------------------------------------------------------------------*/

static void
_assign_vertex_coords(MEDCouplingUMesh   *med_mesh,
                      cs_coord_3_t       *coords   )
{
  const cs_lnum_t  dim  = med_mesh->getMeshDimension();
  const cs_lnum_t n_vtx = med_mesh->getNumberOfNodes();

  /* assign all coordinates */
  /*------------------------*/

  DataArrayDouble *med_coords = DataArrayDouble::New();
  med_coords->alloc(n_vtx, dim);

  for (cs_lnum_t i = 0; i < n_vtx; i++) {
      for (cs_lnum_t j = 0; j < dim; j++)
        med_coords->setIJ(i, j, coords[i][j]);
  }

  med_mesh->setCoords(med_coords);
  med_coords->decrRef();
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief destroy a given intersector
 *
 * \param[in] mi  pointer to the cs_medcoupling_intersector_t struct
 */
/*----------------------------------------------------------------------------*/

void
_destroy_intersector(cs_medcoupling_intersector_t *mi)
{
  BFT_FREE(mi->name);
  BFT_FREE(mi->medfile_path);
  BFT_FREE(mi->interp_method);
  BFT_FREE(mi->source_mesh);
  BFT_FREE(mi->init_coords);
  BFT_FREE(mi->boundary_coords);
  BFT_FREE(mi->init_boundary_coords);
  BFT_FREE(mi->vol_intersect);

  cs_medcoupling_mesh_destroy(mi->local_mesh);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute intersection matrix and update the intersection array
 *
 * \param[in] mi  pointer to the cs_medcoupling_intersector_t struct
 */
/*----------------------------------------------------------------------------*/

void
_compute_intersection_volumes(cs_medcoupling_intersector_t *mi)
{
  /* If local mesh is empty, nothing to do... */
  cs_lnum_t n_elts = mi->local_mesh->n_elts;

  if (n_elts > 0 && mi->matrix_needs_update) {

    /* initialize the pointer */
    for (cs_lnum_t c_id = 0; c_id < cs_glob_mesh->n_cells; c_id++)
      mi->vol_intersect[c_id] = 0.;

    /* Matrix for the target mesh */
    MEDCouplingNormalizedUnstructuredMesh<3,3>
      tMesh_wrapper(mi->local_mesh->med_mesh);

    /* Matrix for the source mesh, based on the bbox of the target mesh */
    const cs_real_t *bbox = mi->local_mesh->bbox;

    const DataArrayIdType *subcells
      = mi->source_mesh->getCellsInBoundingBox(bbox, 1.05);

    MEDCouplingNormalizedUnstructuredMesh<3,3>
      sMesh_wrapper(mi->source_mesh->buildPartOfMySelf(subcells->begin(),
                                                       subcells->end(),
                                                       true));

    /* Compute the intersection matrix between source and target meshes */
    std::vector<std::map<mcIdType, double> > mat;
    INTERP_KERNEL::Interpolation3D interpolator;

    interpolator.interpolateMeshes(sMesh_wrapper,
                                   tMesh_wrapper,
                                   mat,
                                   mi->interp_method);

    /* Loop on the different elements of the target mesh.
     * For each element, we sum all intersected volumes to retrieve the total
     * intersected volume per cell.
     * The iterator map contains two elements:
     * -> first  : which is the index of the intersected cell in source mesh
     * -> second : which the intersection volume
     */
    const cs_lnum_t *connec = mi->local_mesh->new_to_old;
    for (cs_lnum_t e_id = 0; e_id < n_elts; e_id++) {
      cs_lnum_t c_id = connec[e_id];
      for (std::map<mcIdType, double>::iterator it = mat[e_id].begin();
           it != mat[e_id].end();
           ++it)
        mi->vol_intersect[c_id] += it->second;
    }

  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief dump a medcoupling mesh
 *
 * \param[in] m         MEDCouplingUMesh to dump
 * \param[in] prefix    folder where the file is to be written
 * \param[in] filename  name of the file to write
 */
/*----------------------------------------------------------------------------*/

void
_dump_medcoupling_mesh(MEDCouplingUMesh *m,
                       const char       *prefix,
                       const char       *filename)
{
#if defined(WIN32) || defined(_WIN32)
  static const char _dir_separator = '\\';
#else
  static const char _dir_separator = '/';
#endif

  const char _medfiles[] = "medfiles";
  const char _ext[]      = ".med";

  const char *subdir = prefix;

  if (cs_glob_rank_id < 1) {

    /* Sanity check to ensure the subdirectory is defined */
    if (subdir != NULL) {
      if (strlen(subdir) == 0)
        subdir = NULL;
    }
    if (subdir == NULL)
      subdir = _medfiles;

    /* Creat the subdirectory */
    cs_file_mkdir_default(subdir);

    size_t lsdir = strlen(subdir);
    size_t lname = strlen(filename);

    size_t lext = 0;
    if (cs_file_endswith(filename, _ext) == 0)
      lext = strlen(_ext);
    char *fname = NULL;
    BFT_MALLOC(fname, lsdir + lname + lext + 2, char);

    strcpy(fname, subdir);
    fname[lsdir] = _dir_separator;
    fname[lsdir+1] = '\0';
    strcat(fname, filename);
    if (lext != 0)
      strcat(fname, _ext);
    fname[lsdir+lname+lext+1] = '\0';

    WriteUMesh(fname, m, true);
  }
}

#endif

/*===========================================================================*/

BEGIN_C_DECLS

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add a MEDCoupling intersector.
 *
 * \param[in] name             name of the intersector
 * \param[in] medfile_path     path to the MED file
 * \param[in] interp_method    interpolation method (P0P0, P1P0, ..)
 * \param[in] select_criteria  selection criteria
 *
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_add(const char  *name,
                               const char  *medfile_path,
                               const char  *interp_method,
                               const char  *select_criteria)
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(name);
  CS_NO_WARN_IF_UNUSED(medfile_path);
  CS_NO_WARN_IF_UNUSED(interp_method);
  CS_NO_WARN_IF_UNUSED(select_criteria);

  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  if (_n_intersects == 0)
    BFT_MALLOC(_intersects, _n_intersects + 1, cs_medcoupling_intersector_t *);
  else
    BFT_REALLOC(_intersects, _n_intersects + 1, cs_medcoupling_intersector_t *);

  cs_medcoupling_intersector_t *mi = _create_intersector();
  _allocate_intersector(mi,
                        name,
                        medfile_path,
                        interp_method,
                        select_criteria);

  _intersects[_n_intersects] = mi;

  _n_intersects++;
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroy a given MEDCoupling intersector.
 *
 * \param[in]  mi  pointer to the cs_medcoupling_intersector_t struct
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_destroy(cs_medcoupling_intersector_t  *mi)
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  _destroy_intersector(mi);

  BFT_FREE(mi);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Free all allocated intersectors.
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_destroy_all(void)
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  for (int i = 0; i < _n_intersects; i++)
    cs_medcoupling_intersector_destroy(_intersects[i]);

  BFT_FREE(_intersects);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Get a MEDCoupling intersector using its id.
 *
 * \param[in] id  id of the intersector
 *
 * \return pointer to the cs_medcoupling_intersector_t or NULL if not found
 */
/*----------------------------------------------------------------------------*/

cs_medcoupling_intersector_t *
cs_medcoupling_intersector_by_id(int id)
{
  cs_medcoupling_intersector_t *mi = NULL;

  if (id > -1 && id < _n_intersects)
    mi = _intersects[id];

  return mi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Get an intersector by name
 *
 * \param[in] name  name of the intersector
 *
 * \return pointer to the cs_medcoupling_intersector_t or NULL if not found
 */
/*----------------------------------------------------------------------------*/

cs_medcoupling_intersector_t *
cs_medcoupling_intersector_by_name(const char  *name)
{
  cs_medcoupling_intersector_t *mi = NULL;

  if (_n_intersects > 0) {
    for (int i = 0; i < _n_intersects; i++) {
      if (strcmp(name, _intersects[i]->name) == 0) {
        mi = _intersects[i];
        break;
      }
    }
  }

  return mi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the intersection volumes between the source mesh and
 * code mesh
 *
 * \param[in] mi            pointer to the cs_medcoupling_intersector_t struct
 *
 * \return a pointer to the array containing the intersected volume of each cell
 */
/*----------------------------------------------------------------------------*/

cs_real_t *
cs_medcoupling_intersect_volumes(cs_medcoupling_intersector_t  *mi)
{
  cs_real_t *retval = NULL;

#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  /* Compute intersection */
  _compute_intersection_volumes(mi);

  /* Reset intersector matrix status */
  mi->matrix_needs_update = 0;

  /* Return intersected volumes */
  retval = mi->vol_intersect;
#endif

  return retval;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief translate the mesh using a given vector
 *
 * \param[in] mi           pointer to the cs_medcoupling_intersector_t struct
 * \param[in] translation  translation vector
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_translate(cs_medcoupling_intersector_t  *mi,
                                     cs_real_t             translation[3])
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  CS_NO_WARN_IF_UNUSED(translation);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  mi->source_mesh->translate(translation);
  mi->matrix_needs_update = 1;

  cs_real_t matrix[3][4];

  /* Translation matrix
   *       [1   0   0   Dx]
   *  M =  [0   1   0   Dy]
   *       [0   0   1   Dz]
   */
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 4; j++)
      matrix[i][j] = 0.0;

  for (int i = 0; i < 3; i++) {
    matrix[i][i] = 1.0;
    matrix[i][3] = translation[i];
  }

  /* Update the copy of the mesh coordinates */
  const cs_lnum_t n_vtx = mi->source_mesh->getNumberOfNodes();
  for (cs_lnum_t i = 0; i < n_vtx; i++)
    _transform_coord(matrix, mi->init_coords[i]);

  /* Update of the boundary mesh coordinates
   * and of the initial boundary mesh copy coordinates */
  const cs_lnum_t n_b_vtx = mi->n_b_vertices;
  for (cs_lnum_t i = 0; i < n_b_vtx; i++) {
    _transform_coord(matrix, mi->boundary_coords[i]);
    _transform_coord(matrix, mi->init_boundary_coords[i]);
  }
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief rotate the mesh
 *
 * \param[in] mi         pointer to the cs_medcoupling_intersector_t struct
 * \param[in] invariant  Invariant point
 * \param[in] axis       Rotation axis
 * \param[in] angle      angle (in radians)
 *
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_rotate(cs_medcoupling_intersector_t  *mi,
                                  cs_real_t                      invariant[3],
                                  cs_real_t                      axis[3],
                                  cs_real_t                      angle)
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  CS_NO_WARN_IF_UNUSED(invariant);
  CS_NO_WARN_IF_UNUSED(axis);
  CS_NO_WARN_IF_UNUSED(angle);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  mi->source_mesh->rotate(invariant, axis, angle);
  mi->matrix_needs_update = 1;

  cs_real_t matrix[3][4];
  cs_rotation_matrix(angle, axis, invariant, matrix);

  /* Update the copy of the mesh coordinates */
  const cs_lnum_t n_vtx = mi->source_mesh->getNumberOfNodes();
  for (cs_lnum_t i = 0; i < n_vtx; i++)
    _transform_coord(matrix, mi->init_coords[i]);

  /* Update of the boundary mesh coordinates
   * and of the initial boundary mesh copy coordinates */
  const cs_lnum_t n_b_vtx = mi->n_b_vertices;
  for (cs_lnum_t i = 0; i < n_b_vtx; i++){
    _transform_coord(matrix, mi->boundary_coords[i]);
    _transform_coord(matrix, mi->init_boundary_coords[i]);
  }
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Transform a mesh, but takes as input the initial position of the mesh
 * \brief Transformation is thus applied on the initial coordiantes and the
 * \brief mesh is modified accordingly.
 *
 * \param[in] mi         pointer to the cs_medcoupling_intersector_t struct
 * \param[in] matrix     transformation matrix
 *
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_transform_from_init(cs_medcoupling_intersector_t  *mi,
                                               cs_real_t            matrix[3][4])
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  CS_NO_WARN_IF_UNUSED(matrix);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  cs_coord_3_t *_new_coords = NULL ;
  const cs_lnum_t n_vtx = mi->source_mesh->getNumberOfNodes();
  const cs_lnum_t n_b_vtx = mi->n_b_vertices;

  BFT_MALLOC(_new_coords, n_vtx, cs_coord_3_t);

  /* Compute the new coordinates according
   * to a given transformation matrix */
  for (cs_lnum_t i = 0; i < n_vtx; i++)
    _transform_coord_from_init(matrix, mi->init_coords[i], _new_coords[i]);

  /* Update the boundary mesh also */
  for (cs_lnum_t i = 0; i < n_b_vtx; i++)
    _transform_coord_from_init(matrix,
                               mi->init_boundary_coords[i],
                               mi->boundary_coords[i]);

  /* Assign the new set of coordinates to the MED mesh */
  _assign_vertex_coords(mi->source_mesh, _new_coords);

  mi->matrix_needs_update = 1;

  BFT_FREE(_new_coords);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief dump the mesh of a cs_medcoupling_intersector_t structure
 *
 * \param[in] mi      pointer to the cs_medcoupling_intersector_t struct
 * \param[in] prefix  subdir prefix
 */
/*----------------------------------------------------------------------------*/

void
cs_medcoupling_intersector_dump_mesh(cs_medcoupling_intersector_t  *mi,
                                     const char                    *prefix)
{
#if !defined(HAVE_MEDCOUPLING) || !defined(HAVE_MEDCOUPLING_LOADER)
  CS_NO_WARN_IF_UNUSED(mi);
  CS_NO_WARN_IF_UNUSED(prefix);
  bft_error(__FILE__, __LINE__, 0,
            _("Error: This function cannot be called without "
              "MEDCoupling support.\n"));
#else
  _dump_medcoupling_mesh(mi->source_mesh, prefix, mi->name);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create a new writer that will contains the boundary MED mesh added
 * \brief by the user. The writer_id is stored locally..
 *
 * \param[in]  time_dep > 1 if the writer is transient, else writer is fixed
 */
/*----------------------------------------------------------------------------*/

void
cs_mi_post_init_writer(const char             *case_name,
                       const char             *dir_name,
                       const char             *fmt_name,
                       const char             *fmt_opts,
                       fvm_writer_time_dep_t   time_dep,
                       bool                    output_at_start,
                       bool                    output_at_end,
                       int                     frequency_n,
                       double                  frequency_t)
{
  /* We check if a writer_id has already been defined.*/
  bool writer_exists = false;
  if ( _writer_id != 0)
    writer_exists = true;

  /* If a writer do no already exist, create it */
  if (!writer_exists) {
    int writer_id = cs_post_get_free_writer_id();
    _writer_id = writer_id;

    /* Add writer  */
    cs_post_define_writer(writer_id,       /* writer_id */
                          case_name,       /* writer name */
                          dir_name,        /* directory name */
                          fmt_name,        /* format_name */
                          fmt_opts,        /* format_options */
                          time_dep,
                          output_at_start, /* output_at_start */
                          output_at_end,   /* output_at_end */
                          frequency_n,     /* frequency_n */
                          frequency_t);    /* frequency_t */
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Associate a Medcoupling mesh to the default writer
 *
 * \param[in]  mi  pointer to the associated MedCoupling intersector structure
 */
/*----------------------------------------------------------------------------*/

void
cs_mi_post_add_mesh(cs_medcoupling_intersector_t  *mi)
{
  if (_writer_id == 0)
    bft_error(__FILE__, __LINE__, 0,
              _("No writer was defined for MEDCoupling mesh output\n"
                "cs_medcoupling_intersector_post_init_writer should"
                "be called first.\n"));

  int writer_ids[] = {_writer_id};
  int mi_mesh_id = cs_post_get_free_mesh_id();
  cs_post_define_existing_mesh(mi_mesh_id,
                               mi->ext_mesh,
                               0,
                               true,
                               false,
                               1,
                               writer_ids);

  cs_post_write_meshes(NULL);
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
