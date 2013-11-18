#pragma once
#include "FETransverselyIsotropic.h"

//-----------------------------------------------------------------------------
//! 2D transversely isotropic Mooney-Rivlin

//! This class describes a transversely isotropic matrix where the base material
//! is Mooney-Rivlin. The difference between this material and the FETransIsoMooneyRivlin
//! material is that in this material the fibers lie in the plane that is perpendicular
//! to the transverse axis. 

class FE2DTransIsoMooneyRivlin : public FETransverselyIsotropic
{
	enum { NSTEPS = 12 };	// nr of integration steps

public:
	// material parameters
	double	m_c1;	//!< Mooney-Rivlin parameter c1
	double	m_c2;	//!< Mooney-Rivlin parameter c2

	//--- active contraction stuff ---
	double	m_a[2];
	double	m_ac;
	// -------------------------------

public:
	//! constructor
	FE2DTransIsoMooneyRivlin(FEModel* pfem);
	
	//! calculate deviatoric stress at material point
	virtual mat3ds DevStress(FEMaterialPoint& pt);

	//! calculate deviatoric tangent stiffness at material point
	virtual tens4ds DevTangent(FEMaterialPoint& pt);

	// declare parameter list
	DECLARE_PARAMETER_LIST();

protected:
	static double	m_cth[NSTEPS];
	static double	m_sth[NSTEPS];

	double	m_w[2];
};
