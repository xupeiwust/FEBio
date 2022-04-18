/*This file is part of the FEBio source code and is licensed under the MIT license
listed below.

See Copyright-FEBio.txt for details.

Copyright (c) 2021 University of Utah, The Trustees of Columbia University in
the City of New York, and others.

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
#include "FEElasticEASShellDomain.h"
#include "FEElasticMaterial.h"
#include "FEBodyForce.h"
#include <FECore/log.h>
#include <FECore/FEModel.h>
#include <FECore/FEAnalysis.h>
#include <math.h>
#include <FECore/FESolidDomain.h>
#include <FECore/FELinearSystem.h>
#include "FEBioMech.h"

//-----------------------------------------------------------------------------
FEElasticEASShellDomain::FEElasticEASShellDomain(FEModel* pfem) : FESSIShellDomain(pfem), FEElasticDomain(pfem), m_dofSA(pfem), m_dofR(pfem), m_dof(pfem)
{
    m_pMat = nullptr;

    // TODO: Can this be done in Init, since there is no error checking
    if (pfem)
    {
        m_dofSA.AddVariable(FEBioMech::GetVariableName(FEBioMech::SHELL_ACCELERATION));
        m_dofR.AddVariable(FEBioMech::GetVariableName(FEBioMech::RIGID_ROTATION));
    }
}

//-----------------------------------------------------------------------------
FEElasticEASShellDomain& FEElasticEASShellDomain::operator = (FEElasticEASShellDomain& d)
{
    m_Elem = d.m_Elem;
    m_pMesh = d.m_pMesh;
    return (*this);
}

//-----------------------------------------------------------------------------
// get the total dof list
const FEDofList& FEElasticEASShellDomain::GetDOFList() const
{
	return m_dof;
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::SetMaterial(FEMaterial* pmat)
{
	FEDomain::SetMaterial(pmat);
    m_pMat = dynamic_cast<FESolidMaterial*>(pmat);
}

//-----------------------------------------------------------------------------
bool FEElasticEASShellDomain::Init()
{
    // initialize base class
	FESSIShellDomain::Init();
    
    // set up EAS arrays
	m_nEAS = 7;
	for (int i=0; i<Elements(); ++i)
    {
        FEShellElementNew& el = ShellElement(i);
        int neln = el.Nodes();
        int nint = el.GaussPoints();
        el.m_Kaai.resize(m_nEAS, m_nEAS);
        el.m_fa.resize(m_nEAS, 1);
        el.m_alpha.resize(m_nEAS, 1); el.m_alpha.zero();
        el.m_alphat.resize(m_nEAS, 1); el.m_alphat.zero();
        el.m_alphai.resize(m_nEAS, 1); el.m_alphai.zero();
        el.m_Kua.resize(neln,matrix(3, m_nEAS));
        el.m_Kwa.resize(neln,matrix(3, m_nEAS));
        el.m_E.resize(nint, mat3ds(0, 0, 0, 0, 0, 0));
    }
    
    return true;
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::Activate()
{
    for (int i=0; i<Nodes(); ++i)
    {
        FENode& node = Node(i);
        if (node.HasFlags(FENode::EXCLUDE) == false)
        {
            if (node.m_rid < 0)
            {
                node.set_active(m_dofU[0]);
                node.set_active(m_dofU[1]);
                node.set_active(m_dofU[2]);
                
                if (node.HasFlags(FENode::SHELL))
                {
                    node.set_active(m_dofSU[0]);
                    node.set_active(m_dofSU[1]);
                    node.set_active(m_dofSU[2]);
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
//! Initialize element data
void FEElasticEASShellDomain::PreSolveUpdate(const FETimeInfo& timeInfo)
{
    FESSIShellDomain::PreSolveUpdate(timeInfo);
    const int NE = FEElement::MAX_NODES;
    vec3d x0[NE], xt[NE], r0, rt;
    for (size_t i=0; i<m_Elem.size(); ++i)
    {
        FEShellElementNew& el = m_Elem[i];
        el.m_alphai.zero();
        
        int n = el.GaussPoints();
        for (int j=0; j<n; ++j)
        {
            FEMaterialPoint& mp = *el.GetMaterialPoint(j);
            mp.Update(timeInfo);
        }
    }
}

//-----------------------------------------------------------------------------
// Calculates the forces due to the stress
void FEElasticEASShellDomain::InternalForces(FEGlobalVector& R)
{
    int NS = (int)m_Elem.size();
#pragma omp parallel for shared (NS)
    for (int i=0; i<NS; ++i)
    {
        // element force vector
        vector<double> fe;
        vector<int> lm;
        
        // get the element
		FEShellElementNew& el = m_Elem[i];
        
        // create the element force vector and initialize to zero
        int ndof = 6*el.Nodes();
        fe.assign(ndof, 0);
        
        // calculate element's internal force
        ElementInternalForce(el, fe);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble the residual
        R.Assemble(el.m_node, lm, fe, true);
    }
}

//-----------------------------------------------------------------------------
//! calculates the internal equivalent nodal forces for shell elements
//! Note that we use a one-point gauss integration rule for the thickness
//! integration. This will integrate linear functions exactly.

void FEElasticEASShellDomain::ElementInternalForce(FEShellElementNew& el, vector<double>& fe)
{
    int i, n;
    
    // jacobian matrix determinant
    double detJt;
    
    int nint = el.GaussPoints();
    int neln = el.Nodes();
    
    double*    gw = el.GaussWeights();
    
    vec3d Gcnt[3];
    
    // allocate arrays
    vector<mat3ds> S(nint);
    vector<tens4dmm> C(nint);
    vector<double> EE;
    vector< vector<vec3d>> HU;
    vector< vector<vec3d>> HW;
    matrix NS(neln,16);
    matrix NN(neln,8);
    
    // ANS method: Evaluate collocation strains
    CollocationStrainsANS(el, EE, HU, HW, NS, NN);
    
    // EAS method: Evaluate Kua, Kwa, and Kaa
    // Also evaluate PK2 stress and material tangent using enhanced strain
    EvaluateEAS(el, EE, HU, HW, S, C);
    matrix Kif(7,1);
    Kif = el.m_Kaai*el.m_fa;
    
    vector<matrix> hu(neln, matrix(3,6));
    vector<matrix> hw(neln, matrix(3,6));
    vector<vec3d> Nu(neln);
    vector<vec3d> Nw(neln);
    
    // EAS contribution
    matrix Fu(3,1), Fw(3,1);
    for (i=0; i<neln; ++i)
    {
        Fu = el.m_Kua[i]*Kif;
        Fw = el.m_Kwa[i]*Kif;
        
        // calculate internal force
        // the '-' sign is so that the internal forces get subtracted
        // from the global residual vector
        fe[6*i  ] += Fu(0,0);
        fe[6*i+1] += Fu(1,0);
        fe[6*i+2] += Fu(2,0);
        
        fe[6*i+3] += Fw(0,0);
        fe[6*i+4] += Fw(1,0);
        fe[6*i+5] += Fw(2,0);
    }
    
    // repeat for all integration points
    for (n=0; n<nint; ++n)
    {
        ContraBaseVectors0(el, n, Gcnt);
        
        mat3ds E;
        EvaluateEh(el, n, Gcnt, E, hu, hw, Nu, Nw);
        EvaluateANS(el, n, Gcnt, E, hu, hw, EE, HU, HW);
        
        // evaluate 2nd P-K stress
        matrix SC(6,1);
        mat3dsCntMat61(S[n], Gcnt, SC);
        //        mat3ds S = m_pMat->PK2Stress(E);
        //        mat3dsCntMat61(S, Gcnt, SC);
        
        // calculate the jacobian and multiply by Gauss weight
        detJt = detJ0(el, n)*gw[n];
        
        for (i=0; i<neln; ++i)
        {
            Fu = hu[i]*SC;
            Fw = hw[i]*SC;
            
            // calculate internal force
            // the '-' sign is so that the internal forces get subtracted
            // from the global residual vector
            fe[6*i  ] -= Fu(0,0)*detJt;
            fe[6*i+1] -= Fu(1,0)*detJt;
            fe[6*i+2] -= Fu(2,0)*detJt;
            
            fe[6*i+3] -= Fw(0,0)*detJt;
            fe[6*i+4] -= Fw(1,0)*detJt;
            fe[6*i+5] -= Fw(2,0)*detJt;
        }
    }
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::BodyForce(FEGlobalVector& R, FEBodyForce& BF)
{
    int NS = (int)m_Elem.size();
#pragma omp parallel for
    for (int i=0; i<NS; ++i)
    {
        // element force vector
        vector<double> fe;
        vector<int> lm;
        
        // get the element
		FEShellElementNew& el = m_Elem[i];
        
        // create the element force vector and initialize to zero
        int ndof = 6*el.Nodes();
        fe.assign(ndof, 0);
        
        // apply body forces to shells
        ElementBodyForce(BF, el, fe);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble the residual
        R.Assemble(el.m_node, lm, fe, true);
    }
}

//-----------------------------------------------------------------------------
//! Calculates element body forces for shells

void FEElasticEASShellDomain::ElementBodyForce(FEBodyForce& BF, FEShellElementNew& el, vector<double>& fe)
{
    // integration weights
    double* gw = el.GaussWeights();
    double eta;
    double *M, detJt;
    
    // loop over integration points
    int nint = el.GaussPoints();
    int neln = el.Nodes();
    
    for (int n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		double dens = m_pMat->Density(mp);

        // calculate the jacobian
        detJt = detJ0(el, n)*gw[n];
        
        M  = el.H(n);
        eta = el.gt(n);
        
        // get the force
        vec3d f = BF.force(mp);
        
        for (int i=0; i<neln; ++i)
        {
            vec3d fu = f*(dens*M[i]*(1+eta)/2*detJt);
            vec3d fd = f*(dens*M[i]*(1-eta)/2*detJt);
            
            fe[6*i  ] -= fu.x;
            fe[6*i+1] -= fu.y;
            fe[6*i+2] -= fu.z;
            
            fe[6*i+3] -= fd.x;
            fe[6*i+4] -= fd.y;
            fe[6*i+5] -= fd.z;
        }
    }
}

//-----------------------------------------------------------------------------
// Calculate inertial forces \todo Why is F no longer needed?
void FEElasticEASShellDomain::InertialForces(FEGlobalVector& R, vector<double>& F)
{
    FESolidMaterial* pme = dynamic_cast<FESolidMaterial*>(GetMaterial()); assert(pme);
    
    const int MN = FEElement::MAX_NODES;
    vec3d at[MN], aqt[MN];
    
    // acceleration at integration point
    vec3d a;
    
    // loop over all elements
    int NE = Elements();
    
    for (int iel=0; iel<NE; ++iel)
    {
        vector<double> fe;
        vector<int> lm;
        
        FEShellElement& el = Element(iel);
        
        int nint = el.GaussPoints();
        int neln = el.Nodes();
        
        fe.assign(6*neln,0);
        
        // get the nodal accelerations
        for (int i=0; i<neln; ++i)
        {
            at[i] = m_pMesh->Node(el.m_node[i]).m_at;
            aqt[i] = m_pMesh->Node(el.m_node[i]).get_vec3d(m_dofSA[0], m_dofSA[1], m_dofSA[2]);
        }
        
        // evaluate the element inertial force vector
        for (int n=0; n<nint; ++n)
        {
			FEMaterialPoint& mp = *el.GetMaterialPoint(n);

            double J0 = detJ0(el, n)*el.GaussWeights()[n];
			double d = pme->Density(mp);
            
            // get the acceleration for this integration point
            a = evaluate(el, at, aqt, n);
            
            double* M = el.H(n);
            double eta = el.gt(n);
            
            for (int i=0; i<neln; ++i)
            {
                vec3d fu = a*(d*M[i]*(1+eta)/2*J0);
                vec3d fd = a*(d*M[i]*(1-eta)/2*J0);
                
                fe[6*i  ] -= fu.x;
                fe[6*i+1] -= fu.y;
                fe[6*i+2] -= fu.z;
                
                fe[6*i+3] -= fd.x;
                fe[6*i+4] -= fd.y;
                fe[6*i+5] -= fd.z;
            }
        }
        
        // get the element degrees of freedom
        UnpackLM(el, lm);
        
        // assemble fe into R
        R.Assemble(el.m_node, lm, fe, true);
    }
}

//-----------------------------------------------------------------------------
//! This function calculates the stiffness due to body forces
void FEElasticEASShellDomain::ElementBodyForceStiffness(FEBodyForce& BF, FEShellElementNew &el, matrix &ke)
{
    int i, j, i6, j6;
    int neln = el.Nodes();
    
    // jacobian
    double detJ;
    double *M;
    double* gw = el.GaussWeights();
    mat3ds K;
    
    double Mu[FEElement::MAX_NODES], Md[FEElement::MAX_NODES];
    
    // loop over integration points
    int nint = el.GaussPoints();
    for (int n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        detJ = detJ0(el, n)*gw[n];
		double dens = m_pMat->Density(mp);
        // get the stiffness
        K = BF.stiffness(mp)*dens*detJ;
        
        M = el.H(n);
        
        double eta = el.gt(n);
        
        for (i=0; i<neln; ++i)
        {
            Mu[i] = M[i]*(1+eta)/2;
            Md[i] = M[i]*(1-eta)/2;
        }
        
        for (i=0, i6=0; i<neln; ++i, i6 += 6)
        {
            for (j=0, j6 = 0; j<neln; ++j, j6 += 6)
            {
                mat3d Kuu = K*(Mu[i]*Mu[j]);
                mat3d Kud = K*(Mu[i]*Md[j]);
                mat3d Kdu = K*(Md[i]*Mu[j]);
                mat3d Kdd = K*(Md[i]*Md[j]);
                
                ke[i6  ][j6  ] += Kuu(0,0); ke[i6  ][j6+1] += Kuu(0,1); ke[i6  ][j6+2] += Kuu(0,2);
                ke[i6+1][j6  ] += Kuu(1,0); ke[i6+1][j6+1] += Kuu(1,1); ke[i6+1][j6+2] += Kuu(1,2);
                ke[i6+2][j6  ] += Kuu(2,0); ke[i6+2][j6+1] += Kuu(2,1); ke[i6+2][j6+2] += Kuu(2,2);
                
                ke[i6  ][j6+3] += Kud(0,0); ke[i6  ][j6+4] += Kud(0,1); ke[i6  ][j6+5] += Kud(0,2);
                ke[i6+1][j6+3] += Kud(1,0); ke[i6+1][j6+4] += Kud(1,1); ke[i6+1][j6+5] += Kud(1,2);
                ke[i6+2][j6+3] += Kud(2,0); ke[i6+2][j6+4] += Kud(2,1); ke[i6+2][j6+5] += Kud(2,2);
                
                ke[i6+3][j6  ] += Kdu(0,0); ke[i6+3][j6+1] += Kdu(0,1); ke[i6+3][j6+2] += Kdu(0,2);
                ke[i6+4][j6  ] += Kdu(1,0); ke[i6+4][j6+1] += Kdu(1,1); ke[i6+4][j6+2] += Kdu(1,2);
                ke[i6+5][j6  ] += Kdu(2,0); ke[i6+5][j6+1] += Kdu(2,1); ke[i6+5][j6+2] += Kdu(2,2);
                
                ke[i6+3][j6+3] += Kdd(0,0); ke[i6+3][j6+4] += Kdd(0,1); ke[i6+3][j6+5] += Kdd(0,2);
                ke[i6+4][j6+3] += Kdd(1,0); ke[i6+4][j6+4] += Kdd(1,1); ke[i6+4][j6+5] += Kdd(1,2);
                ke[i6+5][j6+3] += Kdd(2,0); ke[i6+5][j6+4] += Kdd(2,1); ke[i6+5][j6+5] += Kdd(2,2);
            }
        }
    }
}

//-----------------------------------------------------------------------------

void FEElasticEASShellDomain::StiffnessMatrix(FELinearSystem& LS)
{
    // repeat over all shell elements
    int NS = (int)m_Elem.size();
#pragma omp parallel for shared (NS)
    for (int iel=0; iel<NS; ++iel)
    {
		FEShellElement& el = m_Elem[iel];

        // create the element's stiffness matrix
		FEElementMatrix ke(el);
		int ndof = 6*el.Nodes();
        ke.resize(ndof, ndof);
        
        // calculate the element stiffness matrix
        ElementStiffness(iel, ke);
        
        // get the element's LM vector
		vector<int> lm;
		UnpackLM(el, lm);
		ke.SetIndices(lm);
        
        // assemble element matrix in global stiffness matrix
		LS.Assemble(ke);
    }
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::MassMatrix(FELinearSystem& LS, double scale)
{
    // repeat over all solid elements
    int NE = (int)m_Elem.size();
#pragma omp parallel for shared (NE)
    for (int iel=0; iel<NE; ++iel)
    {
		FEShellElementNew& el = m_Elem[iel];
        
        // create the element's stiffness matrix
		FEElementMatrix ke(el);
		int ndof = 6*el.Nodes();
        ke.resize(ndof, ndof);
        ke.zero();
        
        // calculate inertial stiffness
        ElementMassMatrix(el, ke, scale);
        
        // get the element's LM vector
		vector<int> lm;
		UnpackLM(el, lm);
		ke.SetIndices(lm);
        
        // assemble element matrix in global stiffness matrix
		LS.Assemble(ke);
    }
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::BodyForceStiffness(FELinearSystem& LS, FEBodyForce& bf)
{
    // repeat over all shell elements
    int NE = (int)m_Elem.size();
#pragma omp parallel for shared (NE)
    for (int iel=0; iel<NE; ++iel)
    {
		FEShellElementNew& el = m_Elem[iel];
        
        // create the element's stiffness matrix
		FEElementMatrix ke(el);
		int ndof = 6*el.Nodes();
        ke.resize(ndof, ndof);
        ke.zero();
        
        // calculate inertial stiffness
        ElementBodyForceStiffness(bf, el, ke);
        
        // get the element's LM vector
		vector<int> lm;
		UnpackLM(el, lm);
		ke.SetIndices(lm);
        
        // assemble element matrix in global stiffness matrix
		LS.Assemble(ke);
    }
}

//-----------------------------------------------------------------------------
//! Calculates the shell element stiffness matrix

void FEElasticEASShellDomain::ElementStiffness(int iel, matrix& ke)
{
	FEShellElementNew& el = ShellElement(iel);
    
    int i, i6, j, j6, n;
    
    // Get the current element's data
    const int nint = el.GaussPoints();
    const int neln = el.Nodes();
    
    // jacobian matrix determinant
    double detJt;
    
    // weights at gauss points
    const double *gw = el.GaussWeights();
    
    vec3d Gcnt[3];
    
    // allocate arrays
    vector<mat3ds> S(nint);
    vector<tens4dmm> C(nint);
    vector<double> EE;
    vector< vector<vec3d>> HU;
    vector< vector<vec3d>> HW;
    matrix NS(neln,16);
    matrix NN(neln,8);
    
    bool ANS = true;
    //    bool ANS = false;
    
    if (ANS) CollocationStrainsANS(el, EE, HU, HW, NS, NN);
    
    // EAS method: Evaluate Kua, Kwa, and Kaa
    // Also evaluate PK2 stress and material tangent using enhanced strain
    EvaluateEAS(el, EE, HU, HW, S, C);
    
    // calculate element stiffness matrix
    vector<matrix> hu(neln, matrix(3,6));
    vector<matrix> hw(neln, matrix(3,6));
    vector<vec3d> Nu(neln);
    vector<vec3d> Nw(neln);
    
    ke.zero();
    
    matrix KUU(3,3), KUW(3,3), KWU(3,3), KWW(3,3);
    for (i=0, i6=0; i<neln; ++i, i6 += 6)
    {
        for (j=0, j6 = 0; j<neln; ++j, j6 += 6)
        {
            KUU = (el.m_Kua[i]*el.m_Kaai*el.m_Kua[j].transpose());
            KUW = (el.m_Kua[i]*el.m_Kaai*el.m_Kwa[j].transpose());
            KWU = (el.m_Kwa[i]*el.m_Kaai*el.m_Kua[j].transpose());
            KWW = (el.m_Kwa[i]*el.m_Kaai*el.m_Kwa[j].transpose());
            
            ke[i6  ][j6  ] -= KUU(0,0); ke[i6  ][j6+1] -= KUU(0,1); ke[i6  ][j6+2] -= KUU(0,2);
            ke[i6+1][j6  ] -= KUU(1,0); ke[i6+1][j6+1] -= KUU(1,1); ke[i6+1][j6+2] -= KUU(1,2);
            ke[i6+2][j6  ] -= KUU(2,0); ke[i6+2][j6+1] -= KUU(2,1); ke[i6+2][j6+2] -= KUU(2,2);
            
            ke[i6  ][j6+3] -= KUW(0,0); ke[i6  ][j6+4] -= KUW(0,1); ke[i6  ][j6+5] -= KUW(0,2);
            ke[i6+1][j6+3] -= KUW(1,0); ke[i6+1][j6+4] -= KUW(1,1); ke[i6+1][j6+5] -= KUW(1,2);
            ke[i6+2][j6+3] -= KUW(2,0); ke[i6+2][j6+4] -= KUW(2,1); ke[i6+2][j6+5] -= KUW(2,2);
            
            ke[i6+3][j6  ] -= KWU(0,0); ke[i6+3][j6+1] -= KWU(0,1); ke[i6+3][j6+2] -= KWU(0,2);
            ke[i6+4][j6  ] -= KWU(1,0); ke[i6+4][j6+1] -= KWU(1,1); ke[i6+4][j6+2] -= KWU(1,2);
            ke[i6+5][j6  ] -= KWU(2,0); ke[i6+5][j6+1] -= KWU(2,1); ke[i6+5][j6+2] -= KWU(2,2);
            
            ke[i6+3][j6+3] -= KWW(0,0); ke[i6+3][j6+4] -= KWW(0,1); ke[i6+3][j6+5] -= KWW(0,2);
            ke[i6+4][j6+3] -= KWW(1,0); ke[i6+4][j6+4] -= KWW(1,1); ke[i6+4][j6+5] -= KWW(1,2);
            ke[i6+5][j6+3] -= KWW(2,0); ke[i6+5][j6+4] -= KWW(2,1); ke[i6+5][j6+5] -= KWW(2,2);
        }
    }
    
    for (n=0; n<nint; ++n)
    {
        ContraBaseVectors0(el, n, Gcnt);
        
        mat3ds E;
        EvaluateEh(el, n, Gcnt, E, hu, hw, Nu, Nw);
        if (ANS) EvaluateANS(el, n, Gcnt, E, hu, hw, EE, HU, HW);
        
        // calculate the jacobian
        detJt = detJ0(el, n)*gw[n];
        
        // evaluate 2nd P-K stress
        matrix SC(6,1);
        mat3dsCntMat61(S[n], Gcnt, SC);
        //        mat3ds S = m_pMat->PK2Stress(E);
        //        mat3dsCntMat61(S, Gcnt, SC);
        
        // evaluate the material tangent
        matrix CC(6,6);
        tens4dmmCntMat66(C[n], Gcnt, CC);
//        tens4dsCntMat66(C[n], Gcnt, CC);
        //        tens4ds c = m_pMat->MaterialTangent(E);
        //        tens4dsCntMat66(c, Gcnt, CC);
        
        // ------------ constitutive component --------------
        
        for (i=0, i6=0; i<neln; ++i, i6 += 6)
        {
            for (j=0, j6 = 0; j<neln; ++j, j6 += 6)
            {
                matrix KUU(3,3), KUW(3,3), KWU(3,3), KWW(3,3);
                KUU = hu[i]*CC*hu[j].transpose();
                KUW = hu[i]*CC*hw[j].transpose();
                KWU = hw[i]*CC*hu[j].transpose();
                KWW = hw[i]*CC*hw[j].transpose();
                KUU *= detJt; KUW *= detJt; KWU *= detJt; KWW *= detJt;
                
                ke[i6  ][j6  ] += KUU(0,0); ke[i6  ][j6+1] += KUU(0,1); ke[i6  ][j6+2] += KUU(0,2);
                ke[i6+1][j6  ] += KUU(1,0); ke[i6+1][j6+1] += KUU(1,1); ke[i6+1][j6+2] += KUU(1,2);
                ke[i6+2][j6  ] += KUU(2,0); ke[i6+2][j6+1] += KUU(2,1); ke[i6+2][j6+2] += KUU(2,2);
                
                ke[i6  ][j6+3] += KUW(0,0); ke[i6  ][j6+4] += KUW(0,1); ke[i6  ][j6+5] += KUW(0,2);
                ke[i6+1][j6+3] += KUW(1,0); ke[i6+1][j6+4] += KUW(1,1); ke[i6+1][j6+5] += KUW(1,2);
                ke[i6+2][j6+3] += KUW(2,0); ke[i6+2][j6+4] += KUW(2,1); ke[i6+2][j6+5] += KUW(2,2);
                
                ke[i6+3][j6  ] += KWU(0,0); ke[i6+3][j6+1] += KWU(0,1); ke[i6+3][j6+2] += KWU(0,2);
                ke[i6+4][j6  ] += KWU(1,0); ke[i6+4][j6+1] += KWU(1,1); ke[i6+4][j6+2] += KWU(1,2);
                ke[i6+5][j6  ] += KWU(2,0); ke[i6+5][j6+1] += KWU(2,1); ke[i6+5][j6+2] += KWU(2,2);
                
                ke[i6+3][j6+3] += KWW(0,0); ke[i6+3][j6+4] += KWW(0,1); ke[i6+3][j6+5] += KWW(0,2);
                ke[i6+4][j6+3] += KWW(1,0); ke[i6+4][j6+4] += KWW(1,1); ke[i6+4][j6+5] += KWW(1,2);
                ke[i6+5][j6+3] += KWW(2,0); ke[i6+5][j6+4] += KWW(2,1); ke[i6+5][j6+5] += KWW(2,2);
            }
        }
        
        // ------------ initial stress component --------------
        
        for (i=0; i<neln; ++i) {
            for (j=0; j<neln; ++j)
            {
                double Kuu, Kuw, Kwu, Kww;
                if (ANS) {
                    double r = el.gr(n);
                    double s = el.gs(n);
                    double N13uu = ((NS(i,0)*NS(j,1) + NS(j,0)*NS(i,1))*(1-s)+
                                    (NS(i,8)*NS(j,9) + NS(j,8)*NS(i,9))*(1+s))/2;
                    double N23uu = ((NS(i,12)*NS(j,13) + NS(j,12)*NS(i,13))*(1-r)+
                                    (NS(i,4)*NS(j,5) + NS(j,4)*NS(i,5))*(1+r))/2;
                    double N33uu = ((1-r)*(1-s)*NN(i,0)*NN(j,0) +
                                    (1+r)*(1-s)*NN(i,2)*NN(j,2) +
                                    (1+r)*(1+s)*NN(i,4)*NN(j,4) +
                                    (1-r)*(1+s)*NN(i,6)*NN(j,6))/4;
                    Kuu = (SC(0,0)*Nu[i].x*Nu[j].x+
                           SC(1,0)*Nu[i].y*Nu[j].y+
                           SC(2,0)*N33uu+
                           SC(3,0)*(Nu[i].x*Nu[j].y+Nu[j].x*Nu[i].y)+
                           SC(4,0)*N23uu+
                           SC(5,0)*N13uu)*detJt;
                    double N13uw = ((NS(i,0)*NS(j,3) + NS(j,2)*NS(i,1))*(1-s)+
                                    (NS(i,8)*NS(j,11) + NS(j,10)*NS(i,9))*(1+s))/2;
                    double N23uw = ((NS(i,12)*NS(j,15) + NS(j,14)*NS(i,13))*(1-r)+
                                    (NS(i,4)*NS(j,7) + NS(j,6)*NS(i,5))*(1+r))/2;
                    double N33uw = ((1-r)*(1-s)*NN(i,0)*NN(j,1) +
                                    (1+r)*(1-s)*NN(i,2)*NN(j,3) +
                                    (1+r)*(1+s)*NN(i,4)*NN(j,5) +
                                    (1-r)*(1+s)*NN(i,6)*NN(j,7))/4;
                    Kuw = (SC(0,0)*Nu[i].x*Nw[j].x+
                           SC(1,0)*Nu[i].y*Nw[j].y+
                           SC(2,0)*N33uw+
                           SC(3,0)*(Nu[i].x*Nw[j].y+Nu[j].x*Nw[i].y)+
                           SC(4,0)*N23uw+
                           SC(5,0)*N13uw)*detJt;
                    double N13wu = ((NS(i,2)*NS(j,1) + NS(j,0)*NS(i,3))*(1-s)+
                                    (NS(i,10)*NS(j,9) + NS(j,8)*NS(i,11))*(1+s))/2;
                    double N23wu = ((NS(i,14)*NS(j,13) + NS(j,12)*NS(i,15))*(1-r)+
                                    (NS(i,6)*NS(j,5) + NS(j,4)*NS(i,7))*(1+r))/2;
                    double N33wu = ((1-r)*(1-s)*NN(i,1)*NN(j,0) +
                                    (1+r)*(1-s)*NN(i,3)*NN(j,2) +
                                    (1+r)*(1+s)*NN(i,5)*NN(j,4) +
                                    (1-r)*(1+s)*NN(i,7)*NN(j,6))/4;
                    Kwu = (SC(0,0)*Nw[i].x*Nu[j].x+
                           SC(1,0)*Nw[i].y*Nu[j].y+
                           SC(2,0)*N33wu+
                           SC(3,0)*(Nw[i].x*Nu[j].y+Nw[j].x*Nu[i].y)+
                           SC(4,0)*N23wu+
                           SC(5,0)*N13wu)*detJt;
                    double N13ww = ((NS(i,2)*NS(j,3) + NS(j,2)*NS(i,3))*(1-s)+
                                    (NS(i,10)*NS(j,11) + NS(j,10)*NS(i,11))*(1+s))/2;
                    double N23ww = ((NS(i,14)*NS(j,15) + NS(j,14)*NS(i,15))*(1-r)+
                                    (NS(i,6)*NS(j,7) + NS(j,6)*NS(i,7))*(1+r))/2;
                    double N33ww = ((1-r)*(1-s)*NN(i,1)*NN(j,1) +
                                    (1+r)*(1-s)*NN(i,3)*NN(j,3) +
                                    (1+r)*(1+s)*NN(i,5)*NN(j,5) +
                                    (1-r)*(1+s)*NN(i,7)*NN(j,7))/4;
                    Kww = (SC(0,0)*Nw[i].x*Nw[j].x+
                           SC(1,0)*Nw[i].y*Nw[j].y+
                           SC(2,0)*N33ww+
                           SC(3,0)*(Nw[i].x*Nw[j].y+Nw[j].x*Nw[i].y)+
                           SC(4,0)*N23ww+
                           SC(5,0)*N13ww)*detJt;
                }
                else {
                    Kuu = (SC(0,0)*Nu[i].x*Nu[j].x+
                           SC(1,0)*Nu[i].y*Nu[j].y+
                           SC(2,0)*Nu[i].z*Nu[j].z+
                           SC(3,0)*(Nu[i].x*Nu[j].y+Nu[j].x*Nu[i].y)+
                           SC(4,0)*(Nu[i].y*Nu[j].z+Nu[j].y*Nu[i].z)+
                           SC(5,0)*(Nu[i].z*Nu[j].x+Nu[j].z*Nu[i].x))*detJt;
                    Kuw = (SC(0,0)*Nu[i].x*Nw[j].x+
                           SC(1,0)*Nu[i].y*Nw[j].y+
                           SC(2,0)*Nu[i].z*Nw[j].z+
                           SC(3,0)*(Nu[i].x*Nw[j].y+Nu[j].x*Nw[i].y)+
                           SC(4,0)*(Nu[i].y*Nw[j].z+Nu[j].y*Nw[i].z)+
                           SC(5,0)*(Nu[i].z*Nw[j].x+Nu[j].z*Nw[i].x))*detJt;
                    Kwu = (SC(0,0)*Nw[i].x*Nu[j].x+
                           SC(1,0)*Nw[i].y*Nu[j].y+
                           SC(2,0)*Nw[i].z*Nu[j].z+
                           SC(3,0)*(Nw[i].x*Nu[j].y+Nw[j].x*Nu[i].y)+
                           SC(4,0)*(Nw[i].y*Nu[j].z+Nw[j].y*Nu[i].z)+
                           SC(5,0)*(Nw[i].z*Nu[j].x+Nw[j].z*Nu[i].x))*detJt;
                    Kww = (SC(0,0)*Nw[i].x*Nw[j].x+
                           SC(1,0)*Nw[i].y*Nw[j].y+
                           SC(2,0)*Nw[i].z*Nw[j].z+
                           SC(3,0)*(Nw[i].x*Nw[j].y+Nw[j].x*Nw[i].y)+
                           SC(4,0)*(Nw[i].y*Nw[j].z+Nw[j].y*Nw[i].z)+
                           SC(5,0)*(Nw[i].z*Nw[j].x+Nw[j].z*Nw[i].x))*detJt;
                }
                
                // the u-u component
                ke[6*i  ][6*j  ] += Kuu;
                ke[6*i+1][6*j+1] += Kuu;
                ke[6*i+2][6*j+2] += Kuu;
                
                // the u-w component
                ke[6*i  ][6*j+3] += Kuw;
                ke[6*i+1][6*j+4] += Kuw;
                ke[6*i+2][6*j+5] += Kuw;
                
                // the w-u component
                ke[6*i+3][6*j  ] += Kwu;
                ke[6*i+4][6*j+1] += Kwu;
                ke[6*i+5][6*j+2] += Kwu;
                
                // the w-w component
                ke[6*i+3][6*j+3] += Kww;
                ke[6*i+4][6*j+4] += Kww;
                ke[6*i+5][6*j+5] += Kww;
            }
        }
        
    } // end loop over gauss-points
    
}

//-----------------------------------------------------------------------------
//! calculates element inertial stiffness matrix
void FEElasticEASShellDomain::ElementMassMatrix(FEShellElementNew& el, matrix& ke, double a)
{
    // Get the current element's data
    const int nint = el.GaussPoints();
    const int neln = el.Nodes();
    
    // weights at gauss points
    const double *gw = el.GaussWeights();
    
    // calculate element stiffness matrix
    for (int n=0; n<nint; ++n)
    {
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);

		double D = m_pMat->Density(mp);

        // shape functions
        double* M = el.H(n);
        
        // Jacobian
        double J0 = detJ0(el, n)*gw[n];
        
        // parametric coordinate through thickness
        double eta = el.gt(n);
        
        for (int i=0; i<neln; ++i)
            for (int j=0; j<neln; ++j)
            {
                double Kuu = (1+eta)/2*M[i]*(1+eta)/2*M[j]*a*D*J0;
                double Kud = (1+eta)/2*M[i]*(1-eta)/2*M[j]*a*D*J0;
                double Kdu = (1-eta)/2*M[i]*(1+eta)/2*M[j]*a*D*J0;
                double Kdd = (1-eta)/2*M[i]*(1-eta)/2*M[j]*a*D*J0;
                
                // the u-u component
                ke[6*i  ][6*j  ] += Kuu;
                ke[6*i+1][6*j+1] += Kuu;
                ke[6*i+2][6*j+2] += Kuu;
                
                // the u-d component
                ke[6*i  ][6*j+3] += Kud;
                ke[6*i+1][6*j+4] += Kud;
                ke[6*i+2][6*j+5] += Kud;
                
                // the d-u component
                ke[6*i+3][6*j  ] += Kdu;
                ke[6*i+4][6*j+1] += Kdu;
                ke[6*i+5][6*j+2] += Kdu;
                
                // the d-d component
                ke[6*i+3][6*j+3] += Kdd;
                ke[6*i+4][6*j+4] += Kdd;
                ke[6*i+5][6*j+5] += Kdd;
            }
    }
    
}

//-----------------------------------------------------------------------------
//! Calculates body forces for shells

void FEElasticEASShellDomain::ElementBodyForce(FEModel& fem, FEShellElementNew& el, vector<double>& fe)
{
    int NF = fem.ModelLoads();
    for (int nf = 0; nf < NF; ++nf)
    {
        FEBodyForce* pbf = dynamic_cast<FEBodyForce*>(fem.ModelLoad(nf));
        if (pbf)
        {
            // integration weights
            double* gw = el.GaussWeights();
            double eta;
            double *M, detJt;
            
            // loop over integration points
            int nint = el.GaussPoints();
            int neln = el.Nodes();
            
            for (int n=0; n<nint; ++n)
            {
                FEMaterialPoint& mp = *el.GetMaterialPoint(n);
                FEElasticMaterialPoint& pt = *mp.ExtractData<FEElasticMaterialPoint>();
                
                // calculate density in current configuration
				double dens0 = m_pMat->Density(mp);
                double dens = dens0/pt.m_J;
                
                // calculate the jacobian
                detJt = detJ(el, n)*gw[n];
                
                M  = el.H(n);
                eta = el.gt(n);
                
                // get the force
                vec3d f = pbf->force(mp);
                
                for (int i=0; i<neln; ++i)
                {
                    vec3d fu = f*(dens*M[i]*(1+eta)/2);
                    vec3d fd = f*(dens*M[i]*(1-eta)/2);
                    
                    fe[6*i  ] -= fu.x*detJt;
                    fe[6*i+1] -= fu.y*detJt;
                    fe[6*i+2] -= fu.z*detJt;
                    
                    fe[6*i+3] -= fd.x*detJt;
                    fe[6*i+4] -= fd.y*detJt;
                    fe[6*i+5] -= fd.z*detJt;
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Update alpha in EAS method
void FEElasticEASShellDomain::UpdateEAS(vector<double>& ui)
{
    FEMesh& mesh = *GetMesh();
    
    for (int i=0; i<(int) m_Elem.size(); ++i)
    {
        // get the solid element
		FEShellElementNew& el = m_Elem[i];
        
        // number of nodes
        int neln = el.Nodes();
        
        // allocate arrays
        matrix dalpha(m_nEAS,1);
        matrix Du(3,1), Dw(3,1);
        
        // nodal coordinates and EAS vector alpha update
        dalpha = el.m_fa;
        for (int j=0; j<neln; ++j)
        {
            FENode& nj = mesh.Node(el.m_node[j]);
            Du(0,0) = (nj.m_ID[m_dofU[0]] >=0) ? ui[nj.m_ID[m_dofU[0]]] : 0;
            Du(1,0) = (nj.m_ID[m_dofU[1]] >=0) ? ui[nj.m_ID[m_dofU[1]]] : 0;
            Du(2,0) = (nj.m_ID[m_dofU[2]] >=0) ? ui[nj.m_ID[m_dofU[2]]] : 0;
            Dw(0,0) = (nj.m_ID[m_dofSU[0]] >=0) ? ui[nj.m_ID[m_dofSU[0]]] : 0;
            Dw(1,0) = (nj.m_ID[m_dofSU[1]] >=0) ? ui[nj.m_ID[m_dofSU[1]]] : 0;
            Dw(2,0) = (nj.m_ID[m_dofSU[2]] >=0) ? ui[nj.m_ID[m_dofSU[2]]] : 0;
            dalpha += el.m_Kua[j].transpose()*Du + el.m_Kwa[j].transpose()*Dw;
        }
        dalpha = el.m_Kaai*dalpha;
        el.m_alpha = el.m_alphat + el.m_alphai - dalpha;
    }
}

//-----------------------------------------------------------------------------
// Update alpha in EAS method
void FEElasticEASShellDomain::UpdateIncrementsEAS(vector<double>& ui, const bool binc)
{
    FEMesh& mesh = *GetMesh();
    
    for (int i=0; i<(int) m_Elem.size(); ++i)
    {
        // get the solid element
		FEShellElementNew& el = m_Elem[i];
        
        if (binc) {
            // number of nodes
            int neln = el.Nodes();
            
            // allocate arrays
            matrix dalpha(m_nEAS,1);
            matrix Du(3,1), Dw(3,1);
            
            // nodal coordinates and EAS vector alpha update
            dalpha = el.m_fa;
            for (int j=0; j<neln; ++j)
            {
                FENode& nj = mesh.Node(el.m_node[j]);
                Du(0,0) = (nj.m_ID[m_dofU[0]] >=0) ? ui[nj.m_ID[m_dofU[0]]] : 0;
                Du(1,0) = (nj.m_ID[m_dofU[1]] >=0) ? ui[nj.m_ID[m_dofU[1]]] : 0;
                Du(2,0) = (nj.m_ID[m_dofU[2]] >=0) ? ui[nj.m_ID[m_dofU[2]]] : 0;
                Dw(0,0) = (nj.m_ID[m_dofSU[0]] >=0) ? ui[nj.m_ID[m_dofSU[0]]] : 0;
                Dw(1,0) = (nj.m_ID[m_dofSU[1]] >=0) ? ui[nj.m_ID[m_dofSU[1]]] : 0;
                Dw(2,0) = (nj.m_ID[m_dofSU[2]] >=0) ? ui[nj.m_ID[m_dofSU[2]]] : 0;
                dalpha += el.m_Kua[j].transpose()*Du + el.m_Kwa[j].transpose()*Dw;
            }
            dalpha = el.m_Kaai*dalpha;
            el.m_alphai -= dalpha;
        }
        else el.m_alphat += el.m_alphai;
    }
}

//-----------------------------------------------------------------------------
void FEElasticEASShellDomain::Update(const FETimeInfo& tp)
{
	FESSIShellDomain::Update(tp);

    FEMesh& mesh = *GetMesh();
    const int MELN = FEElement::MAX_NODES;
    vec3d r0[MELN], rt[MELN];
    
    int n;
    for (int i=0; i<(int) m_Elem.size(); ++i)
    {
        // get the solid element
		FEShellElementNew& el = m_Elem[i];
        
        // get the number of integration points
        int nint = el.GaussPoints();
        
        // number of nodes
        int neln = el.Nodes();
        
        // nodal coordinates
        for (int j=0; j<neln; ++j)
        {
            FENode& nj = mesh.Node(el.m_node[j]);
            r0[j] = nj.m_r0;
            rt[j] = nj.m_rt;
        }
        
        // loop over the integration points and calculate
        // the stress at the integration point
        for (n=0; n<nint; ++n)
        {
            FEMaterialPoint& mp = *(el.GetMaterialPoint(n));
            FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
            
            // material point coordinates
            // TODO: I'm not entirly happy with this solution
            //         since the material point coordinates are used by most materials.
            pt.m_r0 = el.Evaluate(r0, n);
            pt.m_rt = el.Evaluate(rt, n);
            
            // get the deformation gradient and determinant
            pt.m_J = defgrad(el, pt.m_F, n);
            
            // update specialized material points
            m_pMat->UpdateSpecializedMaterialPoints(mp, tp);
            
            // calculate the stress at this material point
            mat3ds S = m_pMat->PK2Stress(mp, el.m_E[n]);
            pt.m_s = (pt.m_F*S*pt.m_F.transpose()).sym()/pt.m_J;
        }
    }
}
/*{
 FEMesh& mesh = *GetMesh();
 vec3d r0[FEElement::MAX_NODES], rt[FEElement::MAX_NODES];
 
 int n;
 for (int i=0; i<(int) m_Elem.size(); ++i)
 {
 // get the solid element
 FEShellElement& el = m_Elem[i];
 
 // get the number of integration points
 int nint = el.GaussPoints();
 
 // number of nodes
 int neln = el.Nodes();
 
 // nodal coordinates
 for (int j=0; j<neln; ++j)
 {
 r0[j] = mesh.Node(el.m_node[j]).m_r0;
 rt[j] = mesh.Node(el.m_node[j]).m_rt;
 }
 
 // loop over the integration points and calculate
 // the stress at the integration point
 for (n=0; n<nint; ++n)
 {
 FEMaterialPoint& mp = *(el.GetMaterialPoint(n));
 FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
 
 // material point coordinates
 // TODO: I'm not entirly happy with this solution
 //         since the material point coordinates are used by most materials.
 pt.m_r0 = el.Evaluate(r0, n);
 pt.m_rt = el.Evaluate(rt, n);
 
 // get the deformation gradient and determinant
 pt.m_J = defgrad(el, pt.m_F, n);
 
 // calculate the stress at this material point
 pt.m_s = m_pMat->Stress(mp);
 }
 }
 }*/

//-----------------------------------------------------------------------------
//! Unpack the element. That is, copy element data in traits structure
//! Note that for the shell elements the lm order is different compared
//! to the solid element ordering. This is because for shell elements the
//! nodes have six degrees of freedom each, where for solids they only
//! have 3 dofs.
void FEElasticEASShellDomain::UnpackLM(FEElement& el, vector<int>& lm)
{
    int N = el.Nodes();
    lm.resize(N*9);
    for (int i=0; i<N; ++i)
    {
        FENode& node = m_pMesh->Node(el.m_node[i]);
        vector<int>& id = node.m_ID;
        
        // first the displacement dofs
        lm[6*i  ] = id[m_dofU[0]];
        lm[6*i+1] = id[m_dofU[1]];
        lm[6*i+2] = id[m_dofU[2]];
        
        // next the shell displacement dofs
        lm[6*i+3] = id[m_dofSU[0]];
        lm[6*i+4] = id[m_dofSU[1]];
        lm[6*i+5] = id[m_dofSU[2]];
        
        // rigid rotational dofs
        lm[6*N + 3*i  ] = id[m_dofR[0]];
        lm[6*N + 3*i+1] = id[m_dofR[1]];
        lm[6*N + 3*i+2] = id[m_dofR[2]];
    }
}

//-----------------------------------------------------------------------------
//! Generate the G matrix for EAS method
void FEElasticEASShellDomain::GenerateGMatrix(FEShellElementNew& el, const int n, const double Jeta, matrix& G)
{
    vec3d Gcnt[3], Gcov[3];
    CoBaseVectors0(el, n, Gcov);
    ContraBaseVectors0(el, 0, 0, 0, Gcnt);
    double J0 = detJ0(el, 0, 0, 0);
    double Jr = J0/Jeta;
    
    double G00 = Gcov[0]*Gcnt[0];
    double G01 = Gcov[0]*Gcnt[1];
    double G02 = Gcov[0]*Gcnt[2];
    double G10 = Gcov[1]*Gcnt[0];
    double G11 = Gcov[1]*Gcnt[1];
    double G12 = Gcov[1]*Gcnt[2];
    double G20 = Gcov[2]*Gcnt[0];
    double G21 = Gcov[2]*Gcnt[1];
    double G22 = Gcov[2]*Gcnt[2];
    
    matrix T0(6,6);
    T0(0,0) = G00*G00; T0(0,1) = G01*G01; T0(0,2) = G02*G02; T0(0,3) = G00*G01; T0(0,4) = G01*G02; T0(0,5) = G00*G02;
    T0(1,0) = G10*G10; T0(1,1) = G11*G11; T0(1,2) = G12*G12; T0(1,3) = G10*G11; T0(1,4) = G11*G12; T0(1,5) = G10*G12;
    T0(2,0) = G20*G20; T0(2,1) = G21*G21; T0(2,2) = G22*G22; T0(2,3) = G20*G21; T0(2,4) = G21*G22; T0(2,5) = G20*G22;
    T0(3,0) = 2*G00*G10; T0(3,1) = 2*G01*G11; T0(3,2) = 2*G02*G12; T0(3,3) = G00*G11+G01*G10; T0(3,4) = G01*G12+G02*G11; T0(3,5) = G00*G12+G02*G10;
    T0(4,0) = 2*G10*G20; T0(4,1) = 2*G11*G21; T0(4,2) = 2*G12*G22; T0(4,3) = G10*G21+G11*G20; T0(4,4) = G11*G22+G12*G21; T0(4,5) = G10*G22+G12*G20;
    T0(5,0) = 2*G00*G20; T0(5,1) = 2*G01*G21; T0(5,2) = 2*G02*G22; T0(5,3) = G00*G21+G01*G20; T0(5,4) = G01*G22+G02*G21; T0(5,5) = G00*G22+G02*G20;
    
    G.resize(6, m_nEAS);
    double r = el.gr(n);
    double s = el.gs(n);
    double t = el.gt(n);
    
    G(0,0) = r*T0(0,0)*Jr;
    G(1,0) = r*T0(1,0)*Jr;
    G(2,0) = r*T0(2,0)*Jr;
    G(3,0) = r*T0(3,0)*Jr;
    G(4,0) = r*T0(4,0)*Jr;
    G(5,0) = r*T0(5,0)*Jr;
    
    G(0,1) = s*T0(0,1)*Jr;
    G(1,1) = s*T0(1,1)*Jr;
    G(2,1) = s*T0(2,1)*Jr;
    G(3,1) = s*T0(3,1)*Jr;
    G(4,1) = s*T0(4,1)*Jr;
    G(5,1) = s*T0(5,1)*Jr;
    
    G(0,2) = t*T0(0,2)*Jr;
    G(1,2) = t*T0(1,2)*Jr;
    G(2,2) = t*T0(2,2)*Jr;
    G(3,2) = t*T0(3,2)*Jr;
    G(4,2) = t*T0(4,2)*Jr;
    G(5,2) = t*T0(5,2)*Jr;
    
    G(0,3) = r*t*T0(0,2)*Jr;
    G(1,3) = r*t*T0(1,2)*Jr;
    G(2,3) = r*t*T0(2,2)*Jr;
    G(3,3) = r*t*T0(3,2)*Jr;
    G(4,3) = r*t*T0(4,2)*Jr;
    G(5,3) = r*t*T0(5,2)*Jr;
    
    G(0,4) = s*t*T0(0,2)*Jr;
    G(1,4) = s*t*T0(1,2)*Jr;
    G(2,4) = s*t*T0(2,2)*Jr;
    G(3,4) = s*t*T0(3,2)*Jr;
    G(4,4) = s*t*T0(4,2)*Jr;
    G(5,4) = s*t*T0(5,2)*Jr;
    
    G(0,5) = r*T0(0,3)*Jr;
    G(1,5) = r*T0(1,3)*Jr;
    G(2,5) = r*T0(2,3)*Jr;
    G(3,5) = r*T0(3,3)*Jr;
    G(4,5) = r*T0(4,3)*Jr;
    G(5,5) = r*T0(5,3)*Jr;
    
    G(0,6) = s*T0(0,3)*Jr;
    G(1,6) = s*T0(1,3)*Jr;
    G(2,6) = s*T0(2,3)*Jr;
    G(3,6) = s*T0(3,3)*Jr;
    G(4,6) = s*T0(4,3)*Jr;
    G(5,6) = s*T0(5,3)*Jr;
}

//-----------------------------------------------------------------------------
//! Evaluate contravariant components of mat3ds tensor
void FEElasticEASShellDomain::mat3dsCntMat61(const mat3ds s, const vec3d* Gcnt, matrix& S)
{
    S.resize(6, 1);
    S(0,0) = Gcnt[0]*(s*Gcnt[0]);
    S(1,0) = Gcnt[1]*(s*Gcnt[1]);
    S(2,0) = Gcnt[2]*(s*Gcnt[2]);
    S(3,0) = Gcnt[0]*(s*Gcnt[1]);
    S(4,0) = Gcnt[1]*(s*Gcnt[2]);
    S(5,0) = Gcnt[0]*(s*Gcnt[2]);
}


//-----------------------------------------------------------------------------
//! Evaluate contravariant components of tens4ds tensor
//! Cijkl = Gj.(Gi.c.Gl).Gk
void FEElasticEASShellDomain::tens4dsCntMat66(const tens4ds c, const vec3d* Gcnt, matrix& C)
{
    C.resize(6, 6);
    C(0,0) =          Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[0])*Gcnt[0]);  // i=0, j=0, k=0, l=0
    C(0,1) = C(1,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[1]);  // i=0, j=0, k=1, l=1
    C(0,2) = C(2,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[2]);  // i=0, j=0, k=2, l=2
    C(0,3) = C(3,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[0]);  // i=0, j=0, k=0, l=1
    C(0,4) = C(4,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[1]);  // i=0, j=0, k=1, l=2
    C(0,5) = C(5,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=0, k=0, l=2
    C(1,1) =          Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[1])*Gcnt[1]);  // i=1, j=1, k=1, l=1
    C(1,2) = C(2,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[2]);  // i=1, j=1, k=2, l=2
    C(1,3) = C(3,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[1])*Gcnt[0]);  // i=1, j=1, k=0, l=1
    C(1,4) = C(4,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[1]);  // i=1, j=1, k=1, l=2
    C(1,5) = C(5,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[0]);  // i=1, j=1, k=0, l=2
    C(2,2) =          Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[2]);  // i=2, j=2, k=2, l=2
    C(2,3) = C(3,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[1])*Gcnt[0]);  // i=2, j=2, k=0, l=1
    C(2,4) = C(4,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[1]);  // i=2, j=2, k=1, l=2
    C(2,5) = C(5,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[0]);  // i=2, j=2, k=0, l=2
    C(3,3) =          Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[0]);  // i=0, j=1, k=0, l=1
    C(3,4) = C(4,3) = Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[1]);  // i=0, j=1, k=1, l=2
    C(3,5) = C(5,3) = Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=1, k=0, l=2
    C(4,4) =          Gcnt[2]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[1]);  // i=1, j=2, k=1, l=2
    C(4,5) = C(5,4) = Gcnt[2]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[0]);  // i=1, j=2, k=0, l=2
    C(5,5) =          Gcnt[2]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=2, k=0, l=2
    
}

//-----------------------------------------------------------------------------
//! Evaluate contravariant components of tens4dmm tensor
//! Cijkl = Gj.(Gi.c.Gl).Gk
void FEElasticEASShellDomain::tens4dmmCntMat66(const tens4dmm c, const vec3d* Gcnt, matrix& C)
{
    C.resize(6, 6);
    C(0,0) =          Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[0])*Gcnt[0]);  // i=0, j=0, k=0, l=0
    C(0,1) = C(1,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[1]);  // i=0, j=0, k=1, l=1
    C(0,2) = C(2,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[2]);  // i=0, j=0, k=2, l=2
    C(0,3) = C(3,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[0]);  // i=0, j=0, k=0, l=1
    C(0,4) = C(4,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[1]);  // i=0, j=0, k=1, l=2
    C(0,5) = C(5,0) = Gcnt[0]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=0, k=0, l=2
    C(1,1) =          Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[1])*Gcnt[1]);  // i=1, j=1, k=1, l=1
    C(1,2) = C(2,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[2]);  // i=1, j=1, k=2, l=2
    C(1,3) = C(3,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[1])*Gcnt[0]);  // i=1, j=1, k=0, l=1
    C(1,4) = C(4,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[1]);  // i=1, j=1, k=1, l=2
    C(1,5) = C(5,1) = Gcnt[1]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[0]);  // i=1, j=1, k=0, l=2
    C(2,2) =          Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[2]);  // i=2, j=2, k=2, l=2
    C(2,3) = C(3,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[1])*Gcnt[0]);  // i=2, j=2, k=0, l=1
    C(2,4) = C(4,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[1]);  // i=2, j=2, k=1, l=2
    C(2,5) = C(5,2) = Gcnt[2]*(vdotTdotv(Gcnt[2], c, Gcnt[2])*Gcnt[0]);  // i=2, j=2, k=0, l=2
    C(3,3) =          Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[1])*Gcnt[0]);  // i=0, j=1, k=0, l=1
    C(3,4) = C(4,3) = Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[1]);  // i=0, j=1, k=1, l=2
    C(3,5) = C(5,3) = Gcnt[1]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=1, k=0, l=2
    C(4,4) =          Gcnt[2]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[1]);  // i=1, j=2, k=1, l=2
    C(4,5) = C(5,4) = Gcnt[2]*(vdotTdotv(Gcnt[1], c, Gcnt[2])*Gcnt[0]);  // i=1, j=2, k=0, l=2
    C(5,5) =          Gcnt[2]*(vdotTdotv(Gcnt[0], c, Gcnt[2])*Gcnt[0]);  // i=0, j=2, k=0, l=2
    
}

//-----------------------------------------------------------------------------
//! Evaluate the matrices and vectors relevant to the EAS method
void FEElasticEASShellDomain::EvaluateEAS(FEShellElementNew& el, vector<double>& EE,
                                       vector< vector<vec3d>>& HU, vector< vector<vec3d>>& HW,
                                       vector<mat3ds>& S, vector<tens4dmm>& c)
{
    int i, n;
    
    // jacobian matrix determinant
    double detJt;
    
    const double* Mr, *Ms, *M;
    
    int nint = el.GaussPoints();
    int neln = el.Nodes();
    
    vector<matrix> hu(neln, matrix(3,6));
    vector<matrix> hw(neln, matrix(3,6));
    vector<vec3d> Nu(neln);
    vector<vec3d> Nw(neln);
    matrix NS(neln,16);
    matrix NN(neln,8);
    
    double*    gw = el.GaussWeights();
    double eta;
    vec3d Gcnt[3];
    
    // Evaluate fa, Kua, Kwa, and Kaa by integrating over the element
    el.m_fa.zero();
    el.m_Kaai.zero();
    for (i=0; i< neln; ++i) {
        el.m_Kua[i].zero();
        el.m_Kwa[i].zero();
    }
    
    // repeat for all integration points
    for (n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        
        ContraBaseVectors0(el, n, Gcnt);
        mat3ds Ec;
        // Evaluate compatible strain tensor
        EvaluateEh(el, n, Gcnt, Ec, hu, hw, Nu, Nw);
        // Evaluate assumed natural strain (ANS) and substitute it into Ec
        EvaluateANS(el, n, Gcnt, Ec, hu, hw, EE, HU, HW);
        
        // calculate the jacobian
        detJt = detJ0(el, n);
        
        // generate G matrix for EAS method
        matrix G;
        GenerateGMatrix(el, n, detJt, G);
        
        detJt *= gw[n];
        
        // Evaluate enhancing strain ES (covariant components)
        matrix ES(6,1);
        ES = G*el.m_alpha;
        // Evaluate the tensor form of ES
        mat3ds Es = ((Gcnt[0] & Gcnt[0])*ES(0,0) + (Gcnt[1] & Gcnt[1])*ES(1,0) + (Gcnt[2] & Gcnt[2])*ES(2,0) +
                     ((Gcnt[0] & Gcnt[1]) + (Gcnt[1] & Gcnt[0]))*(ES(3,0)/2) +
                     ((Gcnt[1] & Gcnt[2]) + (Gcnt[2] & Gcnt[1]))*(ES(4,0)/2) +
                     ((Gcnt[2] & Gcnt[0]) + (Gcnt[0] & Gcnt[2]))*(ES(5,0)/2)).sym();
        // Evaluate enhanced strain
        el.m_E[n] = Ec + Es;
        
        // get the stress tensor for this integration point and evaluate its contravariant components
        S[n] = m_pMat->PK2Stress(mp, el.m_E[n]);
        matrix SM;
        mat3dsCntMat61(S[n], Gcnt, SM);
        
        // get the material tangent
        c[n] = m_pMat->MaterialTangent(mp, el.m_E[n]);
        // get contravariant components of material tangent
        matrix CC;
        tens4dmmCntMat66(c[n], Gcnt, CC);
//        tens4dsCntMat66(c[n], Gcnt, CC);
        
        // Evaluate fa
        matrix tmp(m_nEAS,1);
        tmp = G.transpose()*SM;
        tmp *= detJt;
        el.m_fa += tmp;
        
        // Evaluate Kaa
        matrix Tmpa(m_nEAS,m_nEAS);
        Tmpa = G.transpose()*CC*G;
        Tmpa *= detJt;
        el.m_Kaai += Tmpa;
        
        eta = el.gt(n);
        Mr = el.Hr(n);
        Ms = el.Hs(n);
        M  = el.H(n);
        
        // Evaluate Kua and Kwa
        matrix Tmp(3,m_nEAS);
        for (i=0; i<neln; ++i)
        {
            Tmp = hu[i]*CC*G;
            Tmp *= detJt;
            el.m_Kua[i] += Tmp;
            Tmp = hw[i]*CC*G;
            Tmp *= detJt;
            el.m_Kwa[i] += Tmp;
        }
    }
    // invert Kaa
    el.m_Kaai = el.m_Kaai.inverse();
}

//-----------------------------------------------------------------------------
//! Evaluate collocation strains for assumed natural strain (ANS) method
void FEElasticEASShellDomain::CollocationStrainsANS(FEShellElementNew& el, vector<double>& E,
                                                 vector< vector<vec3d>>& HU, vector< vector<vec3d>>& HW,
                                                 matrix& NS, matrix& NN)
{
    // ANS method for 4-node quadrilaterials
    if (el.Nodes() == 4) {
        vec3d gcov[3], Gcov[3];
        
        double Mr[FEElement::MAX_NODES], Ms[FEElement::MAX_NODES], M[FEElement::MAX_NODES];
        double r, s, t;
        int neln = el.Nodes();
        double Nur, Nus, Nut;
        double Nwr, Nws, Nwt;
        
        // Shear strains E13, E23
        // point A
        r = 0; s = -1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E13A = (gcov[0]*gcov[2] - Gcov[0]*Gcov[2])/2;
        vector<vec3d> hu13A(neln);
        vector<vec3d> hw13A(neln);
        el.shape_fnc(M, r, s);
        el.shape_deriv(Mr, Ms, r, s);
        for (int i=0; i<neln; ++i) {
            NS(i,0) = Nur = (1+t)/2*Mr[i];
            NS(i,1) = Nut = M[i]/2;
            NS(i,2) = Nwr = (1-t)/2*Mr[i];
            NS(i,3) = Nwt = -M[i]/2;
            hu13A[i] = gcov[0]*Nut + gcov[2]*Nur;
            hw13A[i] = gcov[0]*Nwt + gcov[2]*Nwr;
        }
        
        // point B
        r = 1; s = 0; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E23B = (gcov[1]*gcov[2] - Gcov[1]*Gcov[2])/2;
        vector<vec3d> hu23B(neln);
        vector<vec3d> hw23B(neln);
        el.shape_fnc(M, r, s);
        el.shape_deriv(Mr, Ms, r, s);
        for (int i=0; i<neln; ++i) {
            NS(i,4) = Nus = (1+t)/2*Ms[i];
            NS(i,5) = Nut = M[i]/2;
            NS(i,6) = Nws = (1-t)/2*Ms[i];
            NS(i,7) = Nwt = -M[i]/2;
            hu23B[i] = gcov[2]*Nus + gcov[1]*Nut;
            hw23B[i] = gcov[2]*Nws + gcov[1]*Nwt;
        }
        
        // point C
        r = 0; s = 1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E13C = (gcov[0]*gcov[2] - Gcov[0]*Gcov[2])/2;
        vector<vec3d> hu13C(neln);
        vector<vec3d> hw13C(neln);
        el.shape_fnc(M, r, s);
        el.shape_deriv(Mr, Ms, r, s);
        for (int i=0; i<neln; ++i) {
            NS(i,8) = Nur = (1+t)/2*Mr[i];
            NS(i,9) = Nut = M[i]/2;
            NS(i,10) = Nwr = (1-t)/2*Mr[i];
            NS(i,11) = Nwt = -M[i]/2;
            hu13C[i] = gcov[0]*Nut + gcov[2]*Nur;
            hw13C[i] = gcov[0]*Nwt + gcov[2]*Nwr;
        }
        
        // point D
        r = -1; s = 0; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E23D = (gcov[1]*gcov[2] - Gcov[1]*Gcov[2])/2;
        vector<vec3d> hu23D(neln);
        vector<vec3d> hw23D(neln);
        el.shape_fnc(M, r, s);
        el.shape_deriv(Mr, Ms, r, s);
        for (int i=0; i<neln; ++i) {
            NS(i,12) = Nus = (1+t)/2*Ms[i];
            NS(i,13) = Nut = M[i]/2;
            NS(i,14) = Nws = (1-t)/2*Ms[i];
            NS(i,15) = Nwt = -M[i]/2;
            hu23D[i] = gcov[2]*Nus + gcov[1]*Nut;
            hw23D[i] = gcov[2]*Nws + gcov[1]*Nwt;
        }
        
        // normal strain E33
        // point E
        r = -1; s = -1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E33E = (gcov[2]*gcov[2] - Gcov[2]*Gcov[2])/2;
        vector<vec3d> hu33E(neln);
        vector<vec3d> hw33E(neln);
        el.shape_fnc(M, r, s);
        for (int i=0; i<neln; ++i) {
            NN(i,0) = Nut = M[i]/2;
            NN(i,1) = Nwt = -M[i]/2;
            hu33E[i] = gcov[2]*Nut;
            hw33E[i] = gcov[2]*Nwt;
        }
        
        // point F
        r = 1; s = -1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E33F = (gcov[2]*gcov[2] - Gcov[2]*Gcov[2])/2;
        vector<vec3d> hu33F(neln);
        vector<vec3d> hw33F(neln);
        el.shape_fnc(M, r, s);
        for (int i=0; i<neln; ++i) {
            NN(i,2) = Nut = M[i]/2;
            NN(i,3) = Nwt = -M[i]/2;
            hu33F[i] = gcov[2]*Nut;
            hw33F[i] = gcov[2]*Nwt;
        }
        
        // point G
        r = 1; s = 1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E33G = (gcov[2]*gcov[2] - Gcov[2]*Gcov[2])/2;
        vector<vec3d> hu33G(neln);
        vector<vec3d> hw33G(neln);
        el.shape_fnc(M, r, s);
        for (int i=0; i<neln; ++i) {
            NN(i,4) = Nut = M[i]/2;
            NN(i,5) = Nwt = -M[i]/2;
            hu33G[i] = gcov[2]*Nut;
            hw33G[i] = gcov[2]*Nwt;
        }
        
        // point H
        r = -1; s = 1; t = 0;
        CoBaseVectors(el, r, s, t, gcov);
        CoBaseVectors0(el, r, s, t, Gcov);
        double E33H = (gcov[2]*gcov[2] - Gcov[2]*Gcov[2])/2;
        vector<vec3d> hu33H(neln);
        vector<vec3d> hw33H(neln);
        el.shape_fnc(M, r, s);
        for (int i=0; i<neln; ++i) {
            NN(i,6) = Nut = M[i]/2;
            NN(i,7) = Nwt = -M[i]/2;
            hu33H[i] = gcov[2]*Nut;
            hw33H[i] = gcov[2]*Nwt;
        }
        // return the results in aggregated format
        E.resize(8);
        E[0] = E13A; E[1] = E23B; E[2] = E13C; E[3] = E23D;
        E[4] = E33E; E[5] = E33F; E[6] = E33G; E[7] = E33H;
        HU.resize(8,vector<vec3d>(neln)); HW.resize(8,vector<vec3d>(neln));
        for (int i=0; i<neln; ++i) {
            HU[0] = hu13A; HU[1] = hu23B; HU[2] = hu13C; HU[3] = hu23D;
            HU[4] = hu33E; HU[5] = hu33F; HU[6] = hu33G; HU[7] = hu33H;
            HW[0] = hw13A; HW[1] = hw23B; HW[2] = hw13C; HW[3] = hw23D;
            HW[4] = hw33E; HW[5] = hw33F; HW[6] = hw33G; HW[7] = hw33H;
        }
    }
}

//-----------------------------------------------------------------------------
//! Evaluate assumed natural strain (ANS)
void FEElasticEASShellDomain::EvaluateANS(FEShellElementNew& el, const int n, const vec3d* Gcnt,
                                       mat3ds& Ec, vector<matrix>& hu, vector<matrix>& hw,
                                       vector<double>& E, vector< vector<vec3d>>& HU, vector< vector<vec3d>>& HW)
{
    // ANS method for 4-node quadrilaterials
    if (el.Nodes() == 4) {
        vec3d Gcov[3];
        int neln = el.Nodes();
        
        double E13A = E[0]; double E23B = E[1];
        double E13C = E[2]; double E23D = E[3];
        double E33E = E[4]; double E33F = E[5];
        double E33G = E[6]; double E33H = E[7];
        vector<vec3d> hu13A(HU[0]); vector<vec3d> hu23B(HU[1]);
        vector<vec3d> hu13C(HU[2]); vector<vec3d> hu23D(HU[3]);
        vector<vec3d> hu33E(HU[4]); vector<vec3d> hu33F(HU[5]);
        vector<vec3d> hu33G(HU[6]); vector<vec3d> hu33H(HU[7]);
        vector<vec3d> hw13A(HW[0]); vector<vec3d> hw23B(HW[1]);
        vector<vec3d> hw13C(HW[2]); vector<vec3d> hw23D(HW[3]);
        vector<vec3d> hw33E(HW[4]); vector<vec3d> hw33F(HW[5]);
        vector<vec3d> hw33G(HW[6]); vector<vec3d> hw33H(HW[7]);
        
        // Evaluate ANS strains
        double r = el.gr(n);
        double s = el.gs(n);
        double E13ANS = ((1-s)*E13A + (1+s)*E13C)/2;
        double E23ANS = ((1-r)*E23D + (1+r)*E23B)/2;
        double E33ANS = ((1-r)*(1-s)*E33E + (1+r)*(1-s)*E33F +
                         (1+r)*(1+s)*E33G + (1-r)*(1+s)*E33H)/4;
        vector<vec3d> hu13ANS(neln), hu23ANS(neln), hu33ANS(neln);
        vector<vec3d> hw13ANS(neln), hw23ANS(neln), hw33ANS(neln);
        for (int i=0; i<neln; ++i) {
            hu13ANS[i] = (hu13A[i]*(1-s) + hu13C[i]*(1+s))/2;
            hw13ANS[i] = (hw13A[i]*(1-s) + hw13C[i]*(1+s))/2;
            hu23ANS[i] = (hu23D[i]*(1-r) + hu23B[i]*(1+r))/2;
            hw23ANS[i] = (hw23D[i]*(1-r) + hw23B[i]*(1+r))/2;
            hu33ANS[i] = (hu33E[i]*(1-r)*(1-s) + hu33F[i]*(1+r)*(1-s) +
                          hu33G[i]*(1+r)*(1+s) + hu33H[i]*(1-r)*(1+s))/4;
            hw33ANS[i] = (hw33E[i]*(1-r)*(1-s) + hw33F[i]*(1+r)*(1-s) +
                          hw33G[i]*(1+r)*(1+s) + hw33H[i]*(1-r)*(1+s))/4;
        }
        
        // Substitute these strain components into Ec
        CoBaseVectors0(el, n, Gcov);
        double E11c = Gcov[0]*(Ec*Gcov[0]);
        double E22c = Gcov[1]*(Ec*Gcov[1]);
        double E12c = Gcov[0]*(Ec*Gcov[1]);
        Ec = ((Gcnt[0] & Gcnt[0])*E11c + (Gcnt[1] & Gcnt[1])*E22c + (Gcnt[2] & Gcnt[2])*E33ANS +
              ((Gcnt[0] & Gcnt[1]) + (Gcnt[1] & Gcnt[0]))*E12c +
              ((Gcnt[1] & Gcnt[2]) + (Gcnt[2] & Gcnt[1]))*E23ANS +
              ((Gcnt[2] & Gcnt[0]) + (Gcnt[0] & Gcnt[2]))*E13ANS).sym();
        for (int i=0; i<neln; ++i) {
            hu[i](0,5) = hu13ANS[i].x; hu[i](1,5) = hu13ANS[i].y; hu[i](2,5) = hu13ANS[i].z;
            hw[i](0,5) = hw13ANS[i].x; hw[i](1,5) = hw13ANS[i].y; hw[i](2,5) = hw13ANS[i].z;
            hu[i](0,4) = hu23ANS[i].x; hu[i](1,4) = hu23ANS[i].y; hu[i](2,4) = hu23ANS[i].z;
            hw[i](0,4) = hw23ANS[i].x; hw[i](1,4) = hw23ANS[i].y; hw[i](2,4) = hw23ANS[i].z;
            hu[i](0,2) = hu33ANS[i].x; hu[i](1,2) = hu33ANS[i].y; hu[i](2,2) = hu33ANS[i].z;
            hw[i](0,2) = hw33ANS[i].x; hw[i](1,2) = hw33ANS[i].y; hw[i](2,2) = hw33ANS[i].z;
        }
    }
}

//-----------------------------------------------------------------------------
//! Evaluate strain E and matrix hu and hw
void FEElasticEASShellDomain::EvaluateEh(FEShellElementNew& el, const int n, const vec3d* Gcnt, mat3ds& E,
                                      vector<matrix>& hu, vector<matrix>& hw, vector<vec3d>& Nu, vector<vec3d>& Nw)
{
    const double* Mr, *Ms, *M;
    vec3d gcov[3];
    int neln = el.Nodes();
    
    FEMaterialPoint& mp = *(el.GetMaterialPoint(n));
    FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
    
    E = pt.Strain();
    
    CoBaseVectors(el, n, gcov);
    double eta = el.gt(n);
    
    Mr = el.Hr(n);
    Ms = el.Hs(n);
    M  = el.H(n);
    
    for (int i=0; i<neln; ++i)
    {
        double Nur = Nu[i].x = (1+eta)/2*Mr[i];
        double Nus = Nu[i].y = (1+eta)/2*Ms[i];
        double Nut = Nu[i].z = M[i]/2;
        double Nwr = Nw[i].x = (1-eta)/2*Mr[i];
        double Nws = Nw[i].y = (1-eta)/2*Ms[i];
        double Nwt = Nw[i].z = -M[i]/2;
        hu[i](0,0) = Nur*gcov[0].x;                 hu[i](1,0) = Nur*gcov[0].y;                 hu[i](2,0) = Nur*gcov[0].z;
        hu[i](0,1) = Nus*gcov[1].x;                 hu[i](1,1) = Nus*gcov[1].y;                 hu[i](2,1) = Nus*gcov[1].z;
        hu[i](0,2) = Nut*gcov[2].x;                 hu[i](1,2) = Nut*gcov[2].y;                 hu[i](2,2) = Nut*gcov[2].z;
        hu[i](0,3) = Nur*gcov[1].x + Nus*gcov[0].x; hu[i](1,3) = Nur*gcov[1].y + Nus*gcov[0].y; hu[i](2,3) = Nur*gcov[1].z + Nus*gcov[0].z;
        hu[i](0,4) = Nus*gcov[2].x + Nut*gcov[1].x; hu[i](1,4) = Nus*gcov[2].y + Nut*gcov[1].y; hu[i](2,4) = Nus*gcov[2].z + Nut*gcov[1].z;
        hu[i](0,5) = Nut*gcov[0].x + Nur*gcov[2].x; hu[i](1,5) = Nut*gcov[0].y + Nur*gcov[2].y; hu[i](2,5) = Nut*gcov[0].z + Nur*gcov[2].z;
        hw[i](0,0) = Nwr*gcov[0].x;                 hw[i](1,0) = Nwr*gcov[0].y;                 hw[i](2,0) = Nwr*gcov[0].z;
        hw[i](0,1) = Nws*gcov[1].x;                 hw[i](1,1) = Nws*gcov[1].y;                 hw[i](2,1) = Nws*gcov[1].z;
        hw[i](0,2) = Nwt*gcov[2].x;                 hw[i](1,2) = Nwt*gcov[2].y;                 hw[i](2,2) = Nwt*gcov[2].z;
        hw[i](0,3) = Nwr*gcov[1].x + Nws*gcov[0].x; hw[i](1,3) = Nwr*gcov[1].y + Nws*gcov[0].y; hw[i](2,3) = Nwr*gcov[1].z + Nws*gcov[0].z;
        hw[i](0,4) = Nws*gcov[2].x + Nwt*gcov[1].x; hw[i](1,4) = Nws*gcov[2].y + Nwt*gcov[1].y; hw[i](2,4) = Nws*gcov[2].z + Nwt*gcov[1].z;
        hw[i](0,5) = Nwt*gcov[0].x + Nwr*gcov[2].x; hw[i](1,5) = Nwt*gcov[0].y + Nwr*gcov[2].y; hw[i](2,5) = Nwt*gcov[0].z + Nwr*gcov[2].z;
    }
}
