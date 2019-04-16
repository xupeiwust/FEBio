/*This file is part of the FEBio source code and is licensed under the MIT license
listed below.

See Copyright-FEBio.txt for details.

Copyright (c) 2019 University of Utah, Columbia University, and others.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.*/

#include "stdafx.h"
#include "FESolver.h"
#include "FEModel.h"
#include "FENodeReorder.h"
#include "DumpStream.h"

REGISTER_SUPER_CLASS(FESolver, FESOLVER_ID);

BEGIN_FECORE_CLASS(FESolver, FECoreBase)
	ADD_PARAMETER(m_msymm    , "symmetric_stiffness");
	ADD_PARAMETER(m_eq_scheme, "equation_scheme");
	ADD_PARAMETER(m_eq_order , "equation_order" );
	ADD_PARAMETER(m_bwopt    , "optimize_bw");
END_FECORE_CLASS();

//-----------------------------------------------------------------------------
FESolver::FESolver(FEModel* fem) : FECoreBase(fem)
{ 
	m_msymm = REAL_SYMMETRIC; // assume symmetric stiffness matrix
	m_niter = 0;

	m_nref = 0;
	
	m_baugment = false;
	m_naug = 0;

	m_neq = 0;

	m_bwopt = 0;

	m_eq_scheme = EQUATION_SCHEME::STAGGERED;
	m_eq_order = EQUATION_ORDER::NORMAL_ORDER;
}

//-----------------------------------------------------------------------------
FESolver::~FESolver()
{
}

//-----------------------------------------------------------------------------
void FESolver::SetEquationScheme(EQUATION_SCHEME scheme)
{
	m_eq_scheme = scheme;
}

//-----------------------------------------------------------------------------
//! set the linear system partitions
void FESolver::SetPartitions(const vector<int>& part)
{
	m_part = part;
}

//-----------------------------------------------------------------------------
//! Get the size of a partition
int FESolver::GetPartitionSize(int partition)
{
	assert((partition >= 0) && (partition < (int)m_part.size()));
	if ((partition >= 0) && (partition < (int)m_part.size())) return m_part[partition];
	else return 0;
}

//-----------------------------------------------------------------------------
//! get the current stiffness matrix
FEGlobalMatrix* FESolver::GetStiffnessMatrix()
{
	return nullptr;
}

//-----------------------------------------------------------------------------
//! get the current load vector
std::vector<double> FESolver::GetLoadVector()
{
	return std::vector<double>();
}

//-----------------------------------------------------------------------------
void FESolver::Clean()
{
}

//-----------------------------------------------------------------------------
// get the linear solver
LinearSolver* FESolver::GetLinearSolver()
{
	return nullptr;
}

//-----------------------------------------------------------------------------
//! Matrix symmetry flag
int FESolver::MatrixSymmetryFlag() const
{ 
	return m_msymm; 
}

//-----------------------------------------------------------------------------
//! get matrix type
Matrix_Type FESolver::MatrixType() const
{
	Matrix_Type mtype;
	switch (m_msymm)
	{
	case REAL_UNSYMMETRIC   : mtype = REAL_UNSYMMETRIC; break;
	case REAL_SYMMETRIC     : mtype = REAL_SYMMETRIC; break;
	case REAL_SYMM_STRUCTURE: mtype = REAL_SYMM_STRUCTURE; break;
	}
	return mtype;
}

//-----------------------------------------------------------------------------
//! This function is called right before SolveStep and should be used to initialize
//! time dependent information and other settings.
bool FESolver::InitStep(double time)
{
	FEModel& fem = *GetFEModel();

	// evaluate load controllers values at current time
	fem.EvaluateLoadControllers(time);

	// evaluate load parameters
	fem.EvaluateLoadParameters();

	// re-validate materials
	// This is necessary since the material parameters can have changed (e.g. via load curves) and thus 
	// a new validation needs to be done to see if the material parameters are still valid. 
	if (fem.ValidateMaterials() == false) return false;

	return true;
}

//-----------------------------------------------------------------------------
//!	This function initializes the equation system.
//! It is assumed that all free dofs up until now have been given an ID >= 0
//! and the fixed or rigid dofs an ID < 0.
//! After this operation the nodal ID array will contain the equation
//! number assigned to the corresponding degree of freedom. To distinguish
//! between free or unconstrained dofs and constrained ones the following rules
//! apply to the ID array:
//!
//!           /
//!          |  >=  0 --> dof j of node i is a free dof
//! ID[i][j] <  == -1 --> dof j of node i is a fixed (no equation assigned too)
//!          |  <  -1 --> dof j of node i is constrained and has equation nr = -ID[i][j]-2
//!           \
//!
bool FESolver::InitEquations()
{
   // get the mesh
	FEModel& fem = *GetFEModel();
	FEMesh& mesh = fem.GetMesh();
    
    // clear partitions
	m_part.clear();

	// reorder the node numbers
	int NN = mesh.Nodes();
	vector<int> P(NN);
    
    // see if we need to optimize the bandwidth
	if (m_bwopt)
	{
		FENodeReorder mod;
		mod.Apply(mesh, P);
	}
	else for (int i = 0; i < NN; ++i) P[i] = i;

	for (int i = 0; i < mesh.Nodes(); ++i)
	{
		FENode& node = mesh.Node(P[i]);
		if (node.HasFlags(FENode::EXCLUDE))
			for (int j = 0; j < (int)node.m_ID.size(); ++j) node.m_ID[j] = -1;
	}

	// assign equations based on allocation scheme
	int neq = 0;
	if (m_eq_scheme == EQUATION_SCHEME::STAGGERED)
	{
		if (m_eq_order == EQUATION_ORDER::NORMAL_ORDER)
		{
			for (int i = 0; i < mesh.Nodes(); ++i)
			{
				FENode& node = mesh.Node(P[i]);
				if (node.HasFlags(FENode::EXCLUDE) == false) {
					for (int j = 0; j < (int)node.m_ID.size(); ++j)
					{
						if (node.is_active(j))
						{
							int bcj = node.get_bc(j);
							if (bcj == DOF_OPEN) { node.m_ID[j] = neq++; }
							else if (bcj == DOF_FIXED) { node.m_ID[j] = -1; }
							else if (bcj == DOF_PRESCRIBED) { node.m_ID[j] = -neq - 2; neq++; }
							else { assert(false); return false; }
						}
						else node.m_ID[j] = -1;
					}
				}
			}
		}
		else
		{
			int NN = mesh.Nodes();
			for (int i = NN-1; i >= 0; --i)
			{
				FENode& node = mesh.Node(P[i]);
				if (node.HasFlags(FENode::EXCLUDE) == false) {
					int dofs = (int)node.m_ID.size();
					for (int j = dofs - 1; j >= 0; --j)
					{
						if (node.is_active(j))
						{
							int bcj = node.get_bc(j);
							if (bcj == DOF_OPEN) { node.m_ID[j] = neq++; }
							else if (bcj == DOF_FIXED) { node.m_ID[j] = -1; }
							else if (bcj == DOF_PRESCRIBED) { node.m_ID[j] = -neq - 2; neq++; }
							else { assert(false); return false; }
						}
						else node.m_ID[j] = -1;
					}
				}
			}
		}

		// assign partition
		m_part.push_back(neq);
	}
	else
	{
		// Assign equations numbers in blocks
		assert(m_eq_scheme == EQUATION_SCHEME::BLOCK);
		DOFS& dofs = fem.GetDOFS();

		if (m_eq_order == EQUATION_ORDER::NORMAL_ORDER)
		{
			for (int nv = 0; nv < dofs.Variables(); ++nv)
			{
				for (int i = 0; i < NN; ++i)
				{
					FENode& node = mesh.Node(P[i]);
					if (node.HasFlags(FENode::EXCLUDE) == false) {
						int n = dofs.GetVariableSize(nv);
						for (int l = 0; l < n; ++l)
						{
							int nl = dofs.GetDOF(nv, l);

							if (node.is_active(nl))
							{
								int bcl = node.get_bc(nl);
								if (bcl == DOF_FIXED) { node.m_ID[nl] = -1; }
								else if (bcl == DOF_OPEN) { node.m_ID[nl] = neq++; }
								else if (bcl == DOF_PRESCRIBED) { node.m_ID[nl] = -neq - 2; neq++; }
								else { assert(false); return false; }
							}
							else node.m_ID[nl] = -1;
						}
					}
				}

				// assign partitions
				if (nv == 0) m_part.push_back(neq);
				else m_part.push_back(neq - m_part[nv - 1]);
			}
		}
		else
		{
			int vars = dofs.Variables();
			for (int nv = vars-1; nv >= 0; --nv)
			{
				for (int i = 0; i <NN; ++i)
				{
					FENode& node = mesh.Node(P[i]);
					if (node.HasFlags(FENode::EXCLUDE) == false) {
						int n = dofs.GetVariableSize(nv);
						for (int l = 0; l < n; ++l)
						{
							int nl = dofs.GetDOF(nv, l);
							if (node.is_active(nl))
							{
								int bcl = node.get_bc(nl);
								if (bcl == DOF_FIXED) { node.m_ID[nl] = -1; }
								else if (bcl == DOF_OPEN) { node.m_ID[nl] = neq++; }
								else if (bcl == DOF_PRESCRIBED) { node.m_ID[nl] = -neq - 2; neq++; }
								else { assert(false); return false; }
							}
							else node.m_ID[nl] = -1;
						}
					}
				}

				// assign partitions
				if (nv == vars-1) m_part.push_back(neq);
				else m_part.push_back(neq - m_part[(vars-1) - nv - 1]);
			}
		}
	}
    
    // store the number of equations
    m_neq = neq;
    
    // All initialization is done
    return true;
}

//-----------------------------------------------------------------------------
//! add equations
void FESolver::AddEquations(int neq, int partition)
{
	m_neq += neq;
	m_part[partition] += neq;
}

//-----------------------------------------------------------------------------
void FESolver::Serialize(DumpStream& ar)
{
	FECoreBase::Serialize(ar);
	ar & m_nrhs & m_niter & m_nref & m_ntotref & m_naug;
}
