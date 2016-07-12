/*!
 * \file fem_wall_distance.cpp
 * \brief Main subroutines for computing the wall distance for the FEM solver.
 * \author E. van der Weide
 * \version 4.1.3 "Cardinal"
 *
 * SU2 Lead Developers: Dr. Francisco Palacios (Francisco.D.Palacios@boeing.com).
 *                      Dr. Thomas D. Economon (economon@stanford.edu).
 *
 * SU2 Developers: Prof. Juan J. Alonso's group at Stanford University.
 *                 Prof. Piero Colonna's group at Delft University of Technology.
 *                 Prof. Nicolas R. Gauger's group at Kaiserslautern University of Technology.
 *                 Prof. Alberto Guardone's group at Polytechnic University of Milan.
 *                 Prof. Rafael Palacios' group at Imperial College London.
 *
 * Copyright (C) 2012-2015 SU2, the open-source CFD code.
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/fem_geometry_structure.hpp"
#include "../include/adt_structure.hpp"

/* MKL or BLAS files, if supported. */
#ifdef HAVE_MKL
#include "mkl.h"
#elif HAVE_CBLAS
#include "cblas.h"
#endif

void CMeshFEM_DG::ComputeWall_Distance(CConfig *config) {

  /*--------------------------------------------------------------------------*/
  /*--- Step 1: Create the coordinates and connectivity of the linear      ---*/
  /*---         subelements of the local boundaries that must be taken     ---*/
  /*---         into account in the wall distance computation.             ---*/
  /*--------------------------------------------------------------------------*/

  /* Initialize an array for the mesh points, which eventually contains the
     mapping from the local nodes to the number used in the connectivity of the
     local boundary faces. However, in a first pass it is an indicator whether
     or not a mesh point is on a local wall boundary. */
  vector<unsigned long> meshToSurface(meshPoints.size(), 0);

  /* Define the vectors for the connectivity of the local linear subelements,
     the element ID's, the element type and marker ID's. */
  vector<unsigned long> surfaceConn;
  vector<unsigned long> elemIDs;
  vector<unsigned short> VTK_TypeElem;
  vector<unsigned short> markerIDs;

  /* Loop over the boundary markers. */
  for(unsigned short iMarker=0; iMarker<boundaries.size(); ++iMarker) {
    if( !boundaries[iMarker].periodicBoundary ) {

      /* Check for a viscous wall. */
      if( (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX) ||
          (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL) ) {

        /* Loop over the surface elements of this marker. */
        const vector<CSurfaceElementFEM> &surfElem = boundaries[iMarker].surfElem;
        for(unsigned long i=0; i<surfElem.size(); ++i) {

          /* Set the flag of the mesh points on this surface to true. */
          for(unsigned short j=0; j<surfElem[i].nDOFsGrid; ++j)
            meshToSurface[surfElem[i].DOFsGridFace[j]] = 1;

          /* Determine the necessary data from the corresponding standard face,
             such as the number of linear subfaces, the number of DOFs per
             linear subface and the corresponding local connectivity. */
          const unsigned short ind           = surfElem[i].indStandardElement;
          const unsigned short VTK_Type      = standardBoundaryFacesGrid[ind].GetVTK_Type();
          const unsigned short nSubFaces     = standardBoundaryFacesGrid[ind].GetNSubFaces();
          const unsigned short nDOFsPerFace  = standardBoundaryFacesGrid[ind].GetNDOFsPerSubFace();
          const unsigned short *connSubFaces = standardBoundaryFacesGrid[ind].GetSubFaceConn();

          /* Loop over the number of subfaces and store the required data. */
          unsigned short ii = 0;
          for(unsigned short j=0; j<nSubFaces; ++j) {
            markerIDs.push_back(iMarker);
            VTK_TypeElem.push_back(VTK_Type);
            elemIDs.push_back(i);

            for(unsigned short k=0; k<nDOFsPerFace; ++k, ++ii)
              surfaceConn.push_back(surfElem[i].DOFsGridFace[connSubFaces[ii]]);
          }
        }
      }
    }
  }

  /*--- Create the coordinates of the local points on the viscous surfaces and
        create the final version of the mapping from all volume points to the
        points on the viscous surfaces. ---*/
  vector<su2double> surfaceCoor;
  unsigned long nVertex_SolidWall = 0;

  for(unsigned long i=0; i<meshPoints.size(); ++i) {
    if( meshToSurface[i] ) {
      meshToSurface[i] = nVertex_SolidWall++;

      for(unsigned short k=0; k<nDim; ++k)
        surfaceCoor.push_back(meshPoints[i].coor[k]);
    }
  }

  /*--- Change the surface connectivity, such that it corresponds to
        the entries in surfaceCoor rather than in meshPoints. ---*/
  for(unsigned long i=0; i<surfaceConn.size(); ++i)
    surfaceConn[i] = meshToSurface[surfaceConn[i]];

  /*--------------------------------------------------------------------------*/
  /*--- Step 2: Build the ADT, which is an ADT of bounding boxes of the    ---*/
  /*---         surface elements. A nearest point search does not give     ---*/
  /*---         accurate results, especially not for the integration       ---*/
  /*---         points of the elements close to a wall boundary.           ---*/
  /*--------------------------------------------------------------------------*/

  /* Build the ADT. */
  su2_adtElemClass WallADT(nDim, surfaceCoor, surfaceConn, VTK_TypeElem,
                           markerIDs, elemIDs);

  /* Release the memory of the vectors used to build the ADT. To make sure
     that all the memory is deleted, the swap function is used. */
  vector<unsigned short>().swap(markerIDs);
  vector<unsigned short>().swap(VTK_TypeElem);
  vector<unsigned long>().swap(elemIDs);
  vector<unsigned long>().swap(surfaceConn);
  vector<su2double>().swap(surfaceCoor);

  /*--------------------------------------------------------------------------*/
  /*--- Step 3: Determine the coordinates of the integration points of     ---*/
  /*---         locally owned volume elements and determine the wall       ---*/
  /*---         distance for these integration points.                     ---*/
  /*--------------------------------------------------------------------------*/

  /* Loop over the locally owned volume elements and determine the
     total number of integration points in these elements. */
  unsigned long nIntPoints = 0;
  for(unsigned long l=0; l<nVolElemOwned; ++l) {
    const unsigned short ind = volElem[l].indStandardElement;
    nIntPoints += standardElementsGrid[ind].GetNIntegration();
  }

  /* Allocate the memory for the large vector to store the wall distances. */
  VecWallDistanceElements.resize(nIntPoints);

  /*--- Loop over the owned elements to compute the wall distance
        in the integration points. ---*/
  nIntPoints = 0;
  for(unsigned long l=0; l<nVolElemOwned; ++l) {

    /* Get the required data from the corresponding standard element. */
    const unsigned short ind  = volElem[l].indStandardElement;
    const unsigned short nInt = standardElementsGrid[ind].GetNIntegration();
    const su2double      *lag = standardElementsGrid[ind].GetBasisFunctionsIntegration();

    /* Set the pointer for the wall distance for this element. */
    volElem[l].wallDistance = VecWallDistanceElements.data() + nIntPoints;
    nIntPoints += nInt;

    /* Check for an empty tree. In that case the wall distance is set to zero. */
    if( WallADT.IsEmpty() ) {

      /* Empty tree, i.e. no viscous solid walls present. */
      for(unsigned short i=0; i<nInt; ++i)
        volElem[l].wallDistance[i] = 0.0;
    }
    else {

      /* The tree is not empty, hence the distance must be computed.
         Store the grid DOFs of this element a bit easier. */
      const unsigned short nDOFs = volElem[l].nDOFsGrid;
      const unsigned long  *DOFs = volElem[l].nodeIDsGrid.data();

      /* Loop over the integration points of this element. */
      for(unsigned short i=0; i<nInt; ++i) {

        /* Determine the coordinates of this integration point. */
        const unsigned short jj = i*nDOFs;
        const unsigned short kk = i*nDim;
        su2double coor[3];
        for(unsigned short j=0; j<nDim; ++j) {
          coor[j] = 0.0;
          for(unsigned short k=0; k<nDOFs; ++k)
            coor[j] += lag[jj+k]*meshPoints[DOFs[k]].coor[j];
        }

        /* Compute the wall distance. */
        unsigned short markerID;
        unsigned long  elemID;
        int            rankID;
        su2double      dist;
        WallADT.DetermineNearestElement(coor, dist, markerID, elemID, rankID);

        volElem[l].wallDistance[i] = dist;
      }
    }
  }

  /*--------------------------------------------------------------------------*/
  /*--- Step 4: Determine the coordinates of the integration points of     ---*/
  /*---         locally owned internal matching faces and determine the    ---*/
  /*---         wall distance for these integration points.                ---*/
  /*--------------------------------------------------------------------------*/

  /* Loop over the locally owned internal matching faces and determine the
     total number of integration points in these faces. */
  nIntPoints = 0;
  for(unsigned long l=0; l<matchingFaces.size(); ++l) {
    const unsigned short ind = matchingFaces[l].indStandardElement;
    nIntPoints += standardMatchingFacesGrid[ind].GetNIntegration();
  }

  /* Allocate the memory for the large vector to store the wall distances. */
  VecWallDistanceInternalMatchingFaces.resize(nIntPoints);

  /* Loop over the internal matching faces and determine the wall distances
     in the integration points. */
  nIntPoints = 0;
  for(unsigned long l=0; l<matchingFaces.size(); ++l) {

    /* Get the required data from the corresponding standard element. */
    const unsigned short ind   = matchingFaces[l].indStandardElement;
    const unsigned short nInt  = standardMatchingFacesGrid[ind].GetNIntegration();
    const unsigned short nDOFs = standardMatchingFacesGrid[ind].GetNDOFsFaceSide0();
    const su2double      *lag  = standardMatchingFacesGrid[ind].GetBasisFaceIntegrationSide0();

    /* Set the pointer for the wall distance for this matching face. */
    matchingFaces[l].wallDistance = VecWallDistanceInternalMatchingFaces.data() + nIntPoints;
    nIntPoints += nInt;

    /* Check for an empty tree. In that case the wall distance is set to zero. */
    if( WallADT.IsEmpty() ) {

      /* Empty tree, i.e. no viscous solid walls present. */
      for(unsigned short i=0; i<nInt; ++i)
        matchingFaces[l].wallDistance[i] = 0.0;
    }
    else {

      /* The tree is not empty, hence the distance must be computed.
         Store the grid DOFs of this face a bit easier. */
      const unsigned long *DOFs = matchingFaces[l].DOFsGridFaceSide0;

      /* Loop over the integration points of this face. */
      for(unsigned short i=0; i<nInt; ++i) {

        /* Determine the coordinates of this integration point. */
        const unsigned short jj = i*nDOFs;
        const unsigned short kk = i*nDim;
        su2double coor[3];
        for(unsigned short j=0; j<nDim; ++j) {
          coor[j] = 0.0;
          for(unsigned short k=0; k<nDOFs; ++k)
            coor[j] += lag[jj+k]*meshPoints[DOFs[k]].coor[j];
        }

        /* Compute the wall distance. */
        unsigned short markerID;
        unsigned long  elemID;
        int            rankID;
        su2double      dist;
        WallADT.DetermineNearestElement(coor, dist, markerID, elemID, rankID);

        matchingFaces[l].wallDistance[i] = dist;
      }
    }
  }

  /*--------------------------------------------------------------------------*/
  /*--- Step 5: Determine the coordinates of the integration points of     ---*/
  /*---         locally owned boundary faces and determine the wall        ---*/
  /*---         distance for these integration points.                     ---*/
  /*--------------------------------------------------------------------------*/

  /*--- Loop over the boundary markers. Make sure to exclude the periodic
        boundaries, because these are not physical. ---*/
  for(unsigned short iMarker=0; iMarker<boundaries.size(); ++iMarker) {
    if( !boundaries[iMarker].periodicBoundary ) {

      /* Determine the number of integration points for this boundary. */
      nIntPoints = 0;
      vector<CSurfaceElementFEM> &surfElem = boundaries[iMarker].surfElem;
      for(unsigned long l=0; l<surfElem.size(); ++l) {
        const unsigned short ind = surfElem[l].indStandardElement;
        nIntPoints += standardBoundaryFacesGrid[ind].GetNIntegration();
      }

      /* Allocate the memory for the large vector to store the wall distances. */
      boundaries[iMarker].VecWallDistanceBoundaryFaces.resize(nIntPoints);

      /* Determine whether or not this is a viscous wall boundary. */
      const bool viscousWall = config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX ||
                               config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL;

      /* Loop over the boundary faces and determine the wall distances
         in the integration points. */
      nIntPoints = 0;
      for(unsigned long l=0; l<surfElem.size(); ++l) {

        /* Get the required data from the corresponding standard element. */
        const unsigned short ind   = surfElem[l].indStandardElement;
        const unsigned short nInt  = standardBoundaryFacesGrid[ind].GetNIntegration();
        const unsigned short nDOFs = surfElem[l].nDOFsGrid;
        const su2double      *lag  = standardBoundaryFacesGrid[ind].GetBasisFaceIntegration();

        /* Set the pointer for the wall distance for this boundary face. */
        surfElem[l].wallDistance = boundaries[iMarker].VecWallDistanceBoundaryFaces.data() + nIntPoints;
        nIntPoints += nInt;

        /* Check for an empty tree or a viscous wall.
           In those case the wall distance is set to zero. */
        if(WallADT.IsEmpty() || viscousWall) {

          /* Wall distance must be set to zero. */
          for(unsigned short i=0; i<nInt; ++i)
            surfElem[l].wallDistance[i] = 0.0;
        }
        else {

          /* Not a viscous wall boundary, while viscous walls are present. 
             The distance must be computed. Store the DOFs of the face a bit
             easier. */
          const unsigned long *DOFs = surfElem[l].DOFsGridFace;

          /* Loop over the integration points of this face. */
          for(unsigned short i=0; i<nInt; ++i) {

            /* Determine the coordinates of this integration point. */
            const unsigned short jj = i*nDOFs;
            const unsigned short kk = i*nDim;
            su2double coor[3];
            for(unsigned short j=0; j<nDim; ++j) {
              coor[j] = 0.0;
              for(unsigned short k=0; k<nDOFs; ++k)
                coor[j] += lag[jj+k]*meshPoints[DOFs[k]].coor[j];
            }

            /* Compute the wall distance. */
            unsigned short markerID;
            unsigned long  elemID;
            int            rankID;
            su2double      dist;
            WallADT.DetermineNearestElement(coor, dist, markerID, elemID, rankID);

            surfElem[l].wallDistance[i] = dist;
          }
        }
      }
    }
  }
}
