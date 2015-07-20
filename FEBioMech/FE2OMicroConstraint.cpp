#include "stdafx.h"
#include "FE2OMicroConstraint.h"
#include <FECore/FEModel.h>
#include <FECore/log.h>

//-----------------------------------------------------------------------------
//! constructor
FEMicroFlucSurface::FEMicroFlucSurface(FEMesh* pm) : FESurface(pm)
{
	m_Lm.x = 0.; m_Lm.y = 0.; m_Lm.z = 0.;
	m_pv.x = 0.; m_pv.y = 0.; m_pv.z = 0.;
	m_c.x = 0.;  m_c.y = 0.;  m_c.z = 0.;

	m_Fm.unit(); m_Gm.zero();
}

//-----------------------------------------------------------------------------
bool FEMicroFlucSurface::Init()
{
	// calculate the intial microfluctations across the surface
	m_c = SurfMicrofluc();

	return true;
}

//-----------------------------------------------------------------------------
//! Calculate the initial volume
vec3d FEMicroFlucSurface::SurfMicrofluc()
{
	// Integration of microfluctation field across surface
	vec3d c;
	
	// get the mesh
	FEMesh& mesh = *GetMesh();

	// loop over all elements
	double vol = 0.0;
	int NE = Elements();
	vec3d x[FEElement::MAX_NODES];
	vec3d x0[FEElement::MAX_NODES];

	for (int i=0; i<NE; ++i)
	{
		// get the next element
		FESurfaceElement& el = Element(i);

		// get the nodal coordinates
		int neln = el.Nodes();
		for (int j=0; j<neln; ++j){
			x[j] = mesh.Node(el.m_node[j]).m_rt;
			x0[j] = mesh.Node(el.m_node[j]).m_r0;
		}

		// loop over integration points
		double* w = el.GaussWeights();
		int nint = el.GaussPoints();
		for (int n=0; n<nint; ++n)
		{
			vec3d r = el.eval(x, n);
			vec3d r0 = el.eval(x0, n);

			vec3d u = r - r0;
			mat3d I; I.unit();

			c += (u - (m_Fm - I)*r0 - m_Gm.contractdyad1(r0)*0.5)*w[n];
		}
	}
	
	return c;
}

//-----------------------------------------------------------------------------
BEGIN_PARAMETER_LIST(FE2OMicroConstraint, FENLConstraint);
	ADD_PARAMETER(m_blaugon, FE_PARAM_BOOL  , "laugon" ); 
	ADD_PARAMETER(m_atol   , FE_PARAM_DOUBLE, "augtol" );
	ADD_PARAMETER(m_eps    , FE_PARAM_DOUBLE, "penalty");
END_PARAMETER_LIST();

//-----------------------------------------------------------------------------
//! constructor. Set default parameter values
FE2OMicroConstraint::FE2OMicroConstraint(FEModel* pfem) : FENLConstraint(pfem), m_s(&pfem->GetMesh())
{
	m_eps = 0.0;
	m_atol = 0.0;
	m_blaugon = false;
	m_binit = false;	// will be set to true during activation
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::CopyFrom(FENLConstraint* plc)
{
	// cast to a periodic boundary
	FE2OMicroConstraint& mc = dynamic_cast<FE2OMicroConstraint&>(*plc);

	// copy parameters
	GetParameterList() = mc.GetParameterList();

	// copy nodes
	m_s.m_node = mc.m_s.m_node;

	// create elements
	int NE = mc.m_s.Elements();
	m_s.create(NE);
	for (int i=0; i<NE; ++i) m_s.Element(i) = mc.m_s.Element(i);

	// copy surface data
	m_s.m_Lm = mc.m_s.m_Lm;
	m_s.m_pv  = mc.m_s.m_pv;
	m_s.m_c = mc.m_s.m_c;
	m_s.m_Fm = mc.m_s.m_Fm;
	m_s.m_Gm = mc.m_s.m_Gm;
}

//-----------------------------------------------------------------------------
//! Returns the surface
FESurface* FE2OMicroConstraint::GetSurface(const char* sz)
{
	return &m_s;
}

//-----------------------------------------------------------------------------
//! Initializes data structures. 
void FE2OMicroConstraint::Activate()
{
	// don't forget to call base class
	FENLConstraint::Activate();

	// initialize the surface
	if (m_binit == false) m_s.Init();

	// set flag that initial volume is calculated
	m_binit = true;
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::Residual(FEGlobalVector& R, const FETimePoint& tp)
{
	FEMesh& mesh = *m_s.GetMesh();

	vector<double> fe;
	vector<int> lm;

	// get the lagrange 
	vec3d Lm = m_s.m_Lm;

	// loop over all elements
	int NE = m_s.Elements();
	vec3d x[FEElement::MAX_NODES];
	for (int i=0; i<NE; ++i)
	{
		// get the next element
		FESurfaceElement& el = m_s.Element(i);

		// get the nodal coordinates
		int neln = el.Nodes();
		for (int j=0; j<neln; ++j) x[j] = mesh.Node(el.m_node[j]).m_rt;

		// allocate element residual vector
		int ndof = 3*neln;
		fe.resize(ndof);
		zero(fe);

		// loop over all integration points
		double* w = el.GaussWeights();
		int nint = el.GaussPoints();
		for (int n=0; n<nint; ++n)
		{
			// calculate the tangent vectors
			double* Gr = el.Gr(n);
			double* Gs = el.Gs(n);
			vec3d dxr(0,0,0), dxs(0,0,0);
			for (int j=0; j<neln; ++j) 
			{
				dxr += x[j]*Gr[j];
				dxs += x[j]*Gs[j];
			}

			// evaluate the "normal" vector
			vec3d v = (dxr ^ dxs);
			vec3d f = Lm*w[n]*v.norm();

			// evaluate the element forces
			double* H = el.H(n);
			for (int j=0; j<neln; ++j)
			{
				fe[3*j  ] += H[j]*f.x;
				fe[3*j+1] += H[j]*f.y;
				fe[3*j+2] += H[j]*f.z;
			}
		}

		// get the element's LM vector
		m_s.UnpackLM(el, lm);

		// add element force vector to global force vector
		R.Assemble(el.m_node, lm, fe);
	}
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::StiffnessMatrix(FESolver* psolver, const FETimePoint& tp)
{
	FEMesh& mesh = *m_s.GetMesh();

	// element stiffness matrix
	matrix ke;
	vector<int> lm;
	vector<double> fe;

	// loop over all elements
	int NE = m_s.Elements();
	vec3d x[FEElement::MAX_NODES];
	for (int l=0; l<NE; ++l)
	{
		// get the next element
		FESurfaceElement& el = m_s.Element(l);

		// get the nodal coordinates
		int neln = el.Nodes();
		for (int j=0; j<neln; ++j) x[j] = mesh.Node(el.m_node[j]).m_rt;

		// allocate the stiffness matrix
		int ndof = 3*neln;
		ke.resize(ndof, ndof);
		ke.zero();
		fe.resize(ndof);
		zero(fe);

		// repeat over integration points
		double* w = el.GaussWeights();
		int nint = el.GaussPoints();
		for (int n=0; n<nint; ++n)
		{
			// calculate tangent vectors
			double* N = el.H(n);
			double* Gr = el.Gr(n);
			double* Gs = el.Gs(n);
			vec3d dxr(0,0,0), dxs(0,0,0);
			for (int j=0; j<neln; ++j) 
			{
				dxr += x[j]*Gr[j];
				dxs += x[j]*Gs[j];
			}

			// calculate pressure contribution
			vec3d v = (dxr ^ dxs);

			double vi; 
			double vj;
			for (int i=0; i<neln; ++i)
				for (int j=0; j<neln; ++j)
				{
					vi = N[i]*v.norm();
					vj = N[j]*v.norm();
					ke[3*i  ][3*j  ] += m_eps*vi*vj;
					ke[3*i+1][3*j+1] += m_eps*vi*vj;
					ke[3*i+2][3*j+2] += m_eps*vi*vj;
				}

		
			// calculate displacement contribution
			vec3d qab;
			for (int i=0; i<neln; ++i)
				for (int j=0; j<neln; ++j)
				{
					qab = (-dxs*Gr[j] + dxr*Gs[j])*(N[i]/(2*(dxr ^ dxs).norm()))*w[n]; 

					ke[3*i  ][3*j  ] +=      0;
					ke[3*i  ][3*j+1] +=  qab.z;
					ke[3*i  ][3*j+2] += -qab.y;

					ke[3*i+1][3*j  ] += -qab.z;
					ke[3*i+1][3*j+1] +=      0;
					ke[3*i+1][3*j+2] +=  qab.x;

					ke[3*i+2][3*j  ] +=  qab.y;
					ke[3*i+2][3*j+1] += -qab.x;
					ke[3*i+2][3*j+2] +=      0;
				}
		}


		// get the element's LM vector
		m_s.UnpackLM(el, lm);

		// assemble element matrix in global stiffness matrix
		psolver->AssembleStiffness(el.m_node, lm, ke);
	}
}

//-----------------------------------------------------------------------------
bool FE2OMicroConstraint::Augment(int naug, const FETimePoint& tp)
{
	// make sure we are augmenting
	if ((m_blaugon == false) || (m_atol <= 0.0)) return true;

	felog.printf("\n2O periodic surface microfluctation constraint:\n");

	vec3d Dm = m_s.m_c*m_eps;
	vec3d Lm = m_s.m_pv;
	
	double Dnorm = Dm.norm();
	double Lnorm = Lm.norm();

	double err = Dnorm/Lnorm;

	if (Lnorm == 0)
		err = 0;

	felog.printf("\tpressure vect norm: %lg\n", Lm.norm());
	felog.printf("\tnorm : %lg (%lg)\n", err, m_atol);
	felog.printf("\ttotal microfluc norm: %lg\n", m_s.m_c.norm());

	// check convergence
	if (err < m_atol) return true;

	// update Lagrange multiplier (and pressure variable)
	m_s.m_Lm = Lm;
	m_s.m_pv = Lm + Dm;

	return false;
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::Serialize(DumpFile& ar)
{
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::ShallowCopy(DumpStream& dmp, bool bsave)
{
	if (bsave)
	{
		dmp << m_s.m_Lm;
		dmp << m_s.m_pv;
		dmp << m_s.m_c;
		dmp << m_s.m_Fm;
		dmp << m_s.m_Gm;
	}
	else
	{
		dmp >> m_s.m_Lm;
		dmp >> m_s.m_pv;
		dmp >> m_s.m_c;
		dmp >> m_s.m_Fm;
		dmp >> m_s.m_Gm;
	}
}

//-----------------------------------------------------------------------------
void FE2OMicroConstraint::Reset()
{
}

//-----------------------------------------------------------------------------
// This function is called when the FE model's state needs to be updated.
void FE2OMicroConstraint::Update(const FETimePoint& tp)
{
	// calculate the current volume
	m_s.m_c = m_s.SurfMicrofluc();
	
	// update pressure variable
	m_s.m_pv = m_s.m_Lm + m_s.m_c*m_eps;
}