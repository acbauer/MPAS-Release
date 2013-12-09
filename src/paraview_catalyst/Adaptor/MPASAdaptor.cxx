#include "MPASAdaptor.h"

#include "MPASAdaptorAPIMangling.h"

#include "FortranAdaptorAPI.h"
#include "FortranPythonAdaptorAPI.h"

#include "vtkFloatArray.h"
#include "vtkDoubleArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkCell.h"
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCellType.h"
#include "vtkCPAdaptorAPI.h"
#include "vtkCPPythonAdaptorAPI.h"
#include "vtkCPDataDescription.h"
#include "vtkCPInputDataDescription.h"
#include "vtkCPProcessor.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMPI.h"
#include "vtkMPICommunicator.h"
#include "vtkMPIController.h"
#include "vtkPoints.h"
#include "vtkPointData.h"
#include "vtkUnstructuredGrid.h"

#include "vtkTrivialProducer.h"
#include "vtkXMLPMultiBlockDataWriter.h"

#include <float.h>
#include <sstream>
#include <string>

#include "vtkSmartPointer.h"
#define VTK_CREATE(type, name) \
  vtkSmartPointer<type> name = vtkSmartPointer<type>::New()

using namespace std;

namespace MPAS
{
  int rank;
  int totalRank;

  // Parameters from MPAS renamed for primal mesh use
  int nPrimalCells;		// Number of primal cells
  int nPrimalVerts;		// Number of primal vertices
  int nPrimalVertsPerCell;	// Maximum number of vertices per primal cell
  int nPrimalGhosts;		// Number of ghost cells
  int* primalGhostCell;		// Ghost cell indices
  int* primalGhostHalo;		// Ghost cell levels
  int totalPrimalCells;
  int* nEdgesOnCell;		// Primal cell number of sides
  int* verticesOnCell;		// Point indices for dual cells
  int* vertexMask;              // Valid values for primal cells

  // Parameters from MPAS renamed for dual mesh use
  int nDualCells;		// Number of dual cells
  int nDualVerts;		// Number of dual vertices
  int nDualVertsPerCell;	// Maximum number of vertices per dual cell
  int nDualGhosts;		// Number of dual ghost cells
  int* dualGhostCell;		// Ghost cell indices
  int* dualGhostHalo;		// Ghost cell levels
  int totalDualCells;
  int* cellsOnVertex;		// Point indices for dual cells
  int* cellMask;                // Valid values for dual cells

  int nVertLevels;		// Number of vertex depth levels

  const int PRIMAL = 0;
  const int DUAL   = 1;

  string rankName[] = {"primalRank", "dualRank"};
  string maskName[] = {"primalMask", "dualMask"};

  bool* usePrimalCell;
  bool* useDualCell;

  // Mesh depends on how points are entered
  const int X_Y_NLAYER = 0;     // 2D Cartesian with all layers
                                // 3D cell in 3D space
  const int X_Y_Z_1LAYER = 1;   // 3D spherical with any one layer
                                // 2D cell in 3D space
  const int X_Y_Z_NLAYER = 2;   // 3D spherical with any all layers
                                // 3D cell in 3D space
  const int LON_LAT_1LAYER = 3; // 2D map projection with one layer
                                // 2D cell in 2D space
  const int LON_LAT_NLAYER = 4; // 2D map projection with all layers
                                // 3D cell in 3D space
  int pointType;
  int cellDim;
}

using namespace MPAS;

//////////////////////////////////////////////////////////////////////////
//
// Write the vtm file for debugging
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_write_vtk_(int *timeStep)
{
  vtkMultiBlockDataSet* grid = vtkMultiBlockDataSet::SafeDownCast(
      vtkCPAdaptorAPI::GetCoProcessorData()->
                       GetInputDescriptionByName ("input")->GetGrid ());

  char fname[64];
  sprintf(fname, "mpas_ocean_%04d.vtm", *timeStep);

  vtkTrivialProducer* producer = vtkTrivialProducer::New();
  vtkXMLPMultiBlockDataWriter* writer = vtkXMLPMultiBlockDataWriter::New();

  producer->SetOutput(grid);
  producer->Update();
  writer->SetInputConnection(producer->GetOutputPort());
  writer->SetFileName(fname);
  writer->Update();

  producer->Delete();
  writer->Delete();
}

//////////////////////////////////////////////////////////////////////////
//
// Create the coprocessing grid one time
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_create_grid_(
                           int* nCells_,
                           int* maxEdges_,
                           int* nGhostCell_,
                           int* cellGhosts_,
                           int* cellHalos_,

                           int* nVertices_,
                           int* vertexDegree_,
                           int* nGhostVertex_,
                           int* vertexGhosts_,
                           int* vertexHalos_,

                           int* nVertLevels_,

                           double* xCell_,
                           double* yCell_,
                           double* zCell_,

                           double* xVertex_,
                           double* yVertex_,
                           double* zVertex_,

                           double* lonCell_,
                           double* latCell_,

                           double* lonVertex_,
                           double* latVertex_,

                           int* nEdgesOnCell_,
                           int* cellsOnVertex_,
                           int* vertexMask_,

                           int* verticesOnCell_,
                           int* cellMask_)
{
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &totalRank);

  nEdgesOnCell = nEdgesOnCell_;
  cellsOnVertex = cellsOnVertex_;
  vertexMask = vertexMask_;
  verticesOnCell = verticesOnCell_;
  cellMask = cellMask_;
  nVertLevels = *nVertLevels_;

  // Set number of cells in primal and dual mesh
  nPrimalCells = *nCells_;
  nPrimalVerts = *nVertices_;
  nPrimalVertsPerCell = *maxEdges_;
  nPrimalGhosts = *nGhostCell_;
  primalGhostCell = cellGhosts_;
  primalGhostHalo = cellHalos_;
  totalPrimalCells = nPrimalCells * nVertLevels;

  nDualCells = *nVertices_;
  nDualVerts = *nCells_;
  nDualVertsPerCell = *vertexDegree_;
  nDualGhosts = *nGhostVertex_;
  dualGhostCell = vertexGhosts_;
  dualGhostHalo = vertexHalos_;
  totalDualCells = nDualCells * nVertLevels;

  // HACK: This needs to be set from the simulation
 
  pointType = X_Y_NLAYER;      cellDim = 3;
  //pointType = X_Y_Z_1LAYER;    cellDim = 2;
  //pointType = X_Y_Z_NLAYER;    cellDim = 3;
  //pointType = LON_LAT_1LAYER;  cellDim = 2;
  //pointType = LON_LAT_NLAYER;  cellDim = 3;

  if (pointType == X_Y_NLAYER)
    // For X,Y cartesian with all layers create 3D cells in 3D space
    create_xy3D_grids(xCell_, yCell_,
                      xVertex_, yVertex_,
                      -10000.0);

  else if (pointType == X_Y_Z_1LAYER)
    // For X,Y,Z spherical with any one layer create 2D cells in 3D space
    create_xyz2D_grids(xCell_, yCell_, zCell_,
                       xVertex_, yVertex_, zVertex_);

  else if (pointType == X_Y_Z_NLAYER)
    // For X,Y,Z spherical with all layers create 3D cells in 3D space
    create_xyz3D_grids(xCell_, yCell_, zCell_,
                       xVertex_, yVertex_, zVertex_,
                       -50000.0);

  else if (pointType == LON_LAT_1LAYER)
    // For lon/lat cartesian with any one layer create 2D cells in 3D space
    create_lonlat2D_grids(lonCell_, latCell_,
                          lonVertex_, latVertex_);

  else if (pointType == LON_LAT_NLAYER)
    // For lon/lat cartesian with all layers create #D cells in 3D space
    create_lonlat3D_grids(lonCell_, latCell_,
                          lonVertex_, latVertex_,
                          -0.1);
}

//////////////////////////////////////////////////////////////////////////
//
// Register data for the mesh
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_register_data(
                                     char* fname,
                                     int* dim0,
                                     int* dim1,
                                     double* data)
{
  vtkMultiBlockDataSet* grid = vtkMultiBlockDataSet::SafeDownCast(
      vtkCPAdaptorAPI::GetCoProcessorData()->
                       GetInputDescriptionByName ("input")->GetGrid ());

  int numLevels = *dim0;
  int numCells = *dim1;

  int type;
  bool* useCell;
  if (numCells == nPrimalCells) {
    type = PRIMAL;
    useCell = usePrimalCell;
  }
  else if (numCells == nDualCells) {
    type = DUAL;
    useCell = useDualCell;
  }

  vtkUnstructuredGrid* ugrid =
    vtkUnstructuredGrid::SafeDownCast(grid->GetBlock(type));

  vtkStdString name(fname);
  string::size_type pos = name.find(' ');
  vtkStdString varName = name.substr(0, pos); 
  vtkFloatArray* arr = vtkFloatArray::New();
  arr->SetName(varName.c_str());
  ugrid->GetCellData()->AddArray(arr);

  // 2D cells have a component for every layer
  if (cellDim == 2) {
    arr->SetNumberOfComponents(numLevels);
    arr->Allocate(numCells);
    float* value = new float[numLevels];
    for (int j = 0; j < numCells; j++) {
      if (useCell[j] == 1) {
        int findx = j * numLevels;
        for (int lev = 0; lev < numLevels; lev++)
          value[lev] = (float) data[findx + lev];
        arr->InsertNextTuple(value);
      }
    }
    delete [] value;
  }

  // 3D cells have a component for every cell
  else {
    arr->SetNumberOfComponents(1);
    arr->Allocate(numCells * numLevels);
    for (int j = 0; j < numCells; j++) {
      for (int lev = 0; lev < numLevels; lev++) {
        if (useCell[j] == 1) {
          int findx = j * numLevels + lev;
          arr->InsertNextValue((float) data[findx]);
        }
      }
    }
  }
  arr->Delete();
}

//////////////////////////////////////////////////////////////////////////
//
// Tracer data is 3D where the first dimension is the elements of the group
// and so the index of each variable must be given
// Dim 2 is the number of vertex levels and Dim 3 is the number of cells
// So for each cell, each depth, all variable values
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_register_tracer_data(
                                     int* tindex,
                                     char* fname,
                                     int* dim0,
                                     int* dim1,
                                     int* dim2,
                                     double* data)
{
  vtkMultiBlockDataSet* grid = vtkMultiBlockDataSet::SafeDownCast(
      vtkCPAdaptorAPI::GetCoProcessorData()->
                       GetInputDescriptionByName ("input")->GetGrid ());

  int varIndx = *tindex - 1;
  int numTracers = *dim0;
  int numLevels = *dim1;
  int numCells = *dim2;

  int perCell = numLevels * numTracers;
  int perLevel = numTracers;

  int type;
  bool* useCell;
  if (numCells == nPrimalCells) {
    type = PRIMAL;
    useCell = usePrimalCell;
  }
  else if (numCells == nDualCells) {
    type = DUAL;
    useCell = useDualCell;
  }

  vtkUnstructuredGrid* ugrid =
    vtkUnstructuredGrid::SafeDownCast(grid->GetBlock(type));

  vtkStdString name(fname);
  string::size_type pos = name.find(' ');
  vtkStdString varName = name.substr(0, pos); 
  vtkFloatArray* arr = vtkFloatArray::New();
  arr->SetName(varName.c_str());
  ugrid->GetCellData()->AddArray(arr);

  // 2D cells have a component for every layer
  if (cellDim == 2) {
    arr->SetNumberOfComponents(numLevels);
    arr->Allocate(numCells);
    float* value = new float[numLevels];
    for (int cell = 0; cell < numCells; cell++) {
      if (useCell[cell] == 1) {
        for (int lev = 0; lev < numLevels; lev++) {
          int findx = (cell * perCell) + (lev * perLevel) + varIndx;
          value[lev] = (float) data[findx];
        }
        arr->InsertNextTuple(value);
      }
    }
    delete [] value;
  }

  // 3D cells have a component for every cell
  else {
    arr->SetNumberOfComponents(1);
    arr->Allocate(numCells * numLevels);
    for (int cell = 0; cell < numCells; cell++) {
      for (int lev = 0; lev < numLevels; lev++) {
        if (useCell[cell] == 1) {
          int findx = (cell * perCell) + (lev * perLevel) + varIndx;
          arr->InsertNextValue((float) data[findx]);
        }
      }
    }
  }
  arr->Delete();
}

//////////////////////////////////////////////////////////////////////////
//
// Load data into the cartesian mesh
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_add_data(int* itime,
                                     char* fname,
                                     int* dim0,
                                     int* dim1,
                                     double* data)
{
  vtkMultiBlockDataSet* grid = vtkMultiBlockDataSet::SafeDownCast(
      vtkCPAdaptorAPI::GetCoProcessorData()->
                       GetInputDescriptionByName ("input")->GetGrid ());

  int numLevels = *dim0;
  int numCells = *dim1;

  int type;
  bool* useCell;
  if (numCells == nPrimalCells) {
    type = PRIMAL;
    useCell = usePrimalCell;
  }
  else if (numCells == nDualCells) {
    type = DUAL;
    useCell = useDualCell;
  }

  vtkUnstructuredGrid* ugrid =
    vtkUnstructuredGrid::SafeDownCast(grid->GetBlock(type));

  vtkStdString name(fname);
  string::size_type pos = name.find(' ');
  vtkStdString varName = name.substr(0, pos); 
  vtkFloatArray* arr =  vtkFloatArray::SafeDownCast(
        ugrid->GetCellData()->GetArray(varName.c_str()));
  float* ptr = arr->GetPointer(0);

  // 2D cells have a component for every layer
  if (cellDim == 2) {
    int cindx = 0;
    for (int j = 0; j < numCells; j++) {
      if (useCell[j] == 1) {
        int findx = j * numLevels;
        for (int lev = 0; lev < numLevels; lev++)
          ptr[cindx++] = (float) data[findx + lev];
      }
    }
  }

  // 3D cells have a component for every cell
  else {
    int cindx = 0;
    for (int j = 0; j < numCells; j++) {
      for (int lev = 0; lev < numLevels; lev++) {
        if (useCell[j] == 1) {
          int findx = j * numLevels + lev;
          ptr[cindx++] = (float) data[findx];
        }
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////
//
// Load data into the cartesian mesh
//
//////////////////////////////////////////////////////////////////////////

extern "C" void coprocessor_add_tracer_data(int* itime,
                                            int* tindex,
                                            char* fname,
                                            int* dim0,
                                            int* dim1,
                                            int* dim2,
                                            double* data)
{
  vtkMultiBlockDataSet* grid = vtkMultiBlockDataSet::SafeDownCast(
      vtkCPAdaptorAPI::GetCoProcessorData()->
                       GetInputDescriptionByName ("input")->GetGrid ());

  int varIndx = *tindex - 1;
  int numTracers = *dim0;
  int numLevels = *dim1;
  int numCells = *dim2;

  int perCell = numLevels * numTracers;
  int perLevel = numTracers;

  int type;
  bool* useCell;
  if (numCells == nPrimalCells) {
    type = PRIMAL;
    useCell = usePrimalCell;
  }
  else if (numCells == nDualCells) {
    type = DUAL;
    useCell = useDualCell;
  }

  vtkUnstructuredGrid* ugrid =
    vtkUnstructuredGrid::SafeDownCast(grid->GetBlock(type));

  vtkStdString name(fname);
  string::size_type pos = name.find(' ');
  vtkStdString varName = name.substr(0, pos); 
  vtkFloatArray* arr =  vtkFloatArray::SafeDownCast(
        ugrid->GetCellData()->GetArray(varName.c_str()));
  float* ptr = arr->GetPointer(0);

  // 2D cells have a component for every layer
  if (cellDim == 2) {
    int cindx = 0;
    for (int cell = 0; cell < numCells; cell++) {
      if (useCell[cell] == 1) {
        for (int lev = 0; lev < numLevels; lev++) {
          int findx = (cell * perCell) + (lev * perLevel) + varIndx;
          ptr[cindx++] = (float) data[findx];
        }
      }
    }
  }

  // 3D cells have a component for every cell
  else {
    int cindx = 0;
    for (int cell = 0; cell < numCells; cell++) {
      for (int lev = 0; lev < numLevels; lev++) {
        if (useCell[cell] == 1) {
          int findx = (cell * perCell) + (lev * perLevel) + varIndx;
          ptr[cindx++] = (float) data[findx];
        }
      }
    }
  }
}
