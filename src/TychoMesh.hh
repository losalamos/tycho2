//----------------------------------*-C++-*----------------------------------//
/*!
 * \file   mesh/TychoMesh.hh
 * \author Shawn Pautz
 * \date   Fri Jan 14 16:21:46 2000
 * \brief  TychoMesh class header file.
 */
//---------------------------------------------------------------------------//
// $Id: TychoMesh.hh,v 1.7 2000/04/06 21:45:16 pautz Exp $
//---------------------------------------------------------------------------//

/*   
    Notation convention for the mesh   

    Cell: global index for a Tetrahedron
    Node: global index of a Vertex
    Side: global index of a face side
          each face possibly has two sides
          if a face is on a boundary, then it contains a side
    Vrtx: locally defined with respect to either a cell or face
          if cell: index 0,1,2,3   if face: index 0,1,2
    Face: local indexing of a face from a cell: 0,1,2,3
*/

#ifndef __mesh_TychoMesh_hh__
#define __mesh_TychoMesh_hh__

#include "Mat.hh"
#include "Typedef.hh"
#include "Assert.hh"
#include <map>


class TychoMesh 
{
public:
    // Constructor
    TychoMesh(const std::string &filename);
    
    
    // Get data
    UINT getNCells() const
        { return c_nCells; }
    UINT getNSides() const
        { return c_nSides; }
    UINT getNNodes() const
        { return c_nNodes; }
    double getNodeCoord(UINT node, UINT dim) const
        { return c_nodeCoords(node, dim); }
    UINT getCellNode(UINT cell, UINT vrtx) const
        { return c_cellNodes(cell, vrtx); }
    UINT getAdjCell(UINT cell, UINT face) const
        { return c_adjCell(cell, face); }
    UINT getAdjFace(UINT cell, UINT face) const
        { return c_adjCell(cell, face); }
    UINT getSideCell(UINT side) const
        { return c_sideCell(side); }
    UINT getSide(UINT cell, UINT face) const
        { return c_side(cell, face); }
    UINT getLGSide(const UINT side) const
        { return c_lGSides(side); }
    UINT getGLSide(const UINT side) const
        { return c_gLSides.find(side)->second; }
    UINT getAdjRank(const UINT cell, const UINT face) const
        { return c_adjProc(cell, face); }
    UINT getFaceToCellVrtx(const UINT cell, const UINT face, const UINT fvrtx) const
        { return c_faceToCellVrtx(cell, face, fvrtx); }
    UINT getCellToFaceVrtx(const UINT cell, const UINT face, const UINT cvrtx) const
        { Assert(cvrtx != face);
          return c_cellToFaceVrtx(cell, face, cvrtx); }
    double getOmegaDotN(UINT angle, UINT cell, UINT face) const
        { return c_omegaDotN(angle, cell, face); }
    double getCellVolume(const UINT cell) const
        { return c_cellVolume(cell); }
    double getFaceArea(const UINT cell, const UINT face) const
        { return c_faceArea(cell, face); }
    UINT getNeighborVrtx(const UINT cell, const UINT face, const UINT fvrtx) const
        { return c_neighborVrtx(cell, face, fvrtx); }
    bool isOutgoing(const UINT angle, const UINT cell, const UINT face) const
        { return getOmegaDotN(angle, cell, face) > 0; }
    bool isIncoming(const UINT angle, const UINT cell, const UINT face) const
        { return !isOutgoing(angle, cell, face); }
    UINT getAdjCellFromSide(const UINT side) const
        { return c_adjCellFromSide(side); }
    UINT getAdjFaceFromSide(const UINT side) const
        { return c_adjFaceFromSide(side); }
    
    
    // Arbitrary value to mark any face that lies on a boundary.
    static const UINT BOUNDARY_FACE = UINT64_MAX;
    static const UINT NOT_BOUNDARY_FACE = UINT64_MAX;
    static const UINT BAD_RANK = UINT64_MAX;
    
    
private:
    void readTychoMesh(const std::string &filename);
    Mat2<double> getCellVrtxCoords(UINT cell) const;
    Mat2<double> getFaceVrtxCoords(UINT cell, UINT face) const;
    UINT getCellVrtx(const UINT cell, const UINT node) const;
    
    UINT c_nCells;
    UINT c_nSides;
    UINT c_nNodes;
    Mat2<double> c_nodeCoords; // (node, dim) -> coord
    Mat2<UINT> c_cellNodes; // (cell, vrtx) -> node
    Mat2<UINT> c_adjCell; // (cell, face) -> cell
    Mat2<UINT> c_adjFace; // (cell, face) -> face
    Mat1<UINT> c_sideCell; // side -> cell
    Mat2<UINT> c_side; // (cell, face) -> side
    Mat1<UINT> c_lGSides; // local to global side numbering.
    std::map<UINT, UINT> c_gLSides; // global to local side numbering.
    Mat2<UINT> c_adjProc; // (cell, face) -> adjacent proc
    Mat3<double> c_omegaDotN; // (angle, cell, face) -> omega dot n
    Mat1<double> c_cellVolume; // cell -> volume
    Mat2<double> c_faceArea; // (cell, face) -> area
    Mat3<UINT> c_faceToCellVrtx; // (cell, face, fvrtx) -> cvrtx
    Mat3<UINT> c_cellToFaceVrtx; // (cell, face, cvrtx) -> fvrtx
    Mat3<UINT> c_neighborVrtx; // (cell, face, fvrtx) -> vrtx
    Mat1<UINT> c_adjCellFromSide; // side -> adj cell
    Mat1<UINT> c_adjFaceFromSide; // side -> adj face
};


#endif