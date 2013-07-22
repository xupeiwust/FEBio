#include "stdafx.h"
#include "FEBioControlSection.h"
#include <NumCore/ConjGradIterSolver.h>
#include <FEBioLib/FESolidSolver.h>
#include <FEBioLib/FEExplicitSolidSolver.h>
#include <FEBioLib/FEBiphasicSolver.h>
#include <FEBioLib/FEBiphasicSoluteSolver.h>
#include <FEBioLib/FELinearSolidSolver.h>
#include <FEBioLib/FECoupledHeatSolidSolver.h>
#include <FEBioLib/FEUDGHexDomain.h>
#include <FEBioLib/FEUT4Domain.h>
#include <FEBioLib/SuperLUSolver.h>
#include <FEBioHeat/FEHeatSolver.h>

//-----------------------------------------------------------------------------
FESolver* FEBioControlSection::BuildSolver(int nmod, FEModel& fem)
{
	switch (nmod)
	{
	case FE_SOLID         : return new FESolidSolver           (fem);
	case FE_EXPLICIT_SOLID: return new FEExplicitSolidSolver   (fem);
	case FE_BIPHASIC      : return new FEBiphasicSolver        (fem);
	case FE_POROSOLUTE    : return new FEBiphasicSoluteSolver  (fem);
	case FE_HEAT          : return new FEHeatSolver            (fem);
	case FE_LINEAR_SOLID  : return new FELinearSolidSolver     (fem);
	case FE_HEAT_SOLID    : return new FECoupledHeatSolidSolver(fem);
	default:
		assert(false);
		return 0;
	}
}

//-----------------------------------------------------------------------------
void FEBioControlSection::Parse(XMLTag& tag)
{
	FEModel& fem = *GetFEModel();
	FEAnalysisStep* pstep = GetStep();

	// make sure we have a solver defined
	if (pstep->m_psolver == 0) pstep->m_psolver = BuildSolver(pstep->GetType(), fem);
	FENLSolver* psolver = pstep->m_psolver;

	++tag;
	do
	{
		// first parse common control parameters
		if (ParseCommonParams(tag) == false)
		{
			// next, check the solver parameters
			if (m_pim->ReadParameter(tag, psolver->GetParameterList()) == false)
			{
				throw XMLReader::InvalidTag(tag);
			}
		}

		++tag;
	}
	while (!tag.isend());
}

//-----------------------------------------------------------------------------
// Parse control parameters common to all solvers/modules
bool FEBioControlSection::ParseCommonParams(XMLTag& tag)
{
	FEModel& fem = *GetFEModel();
	FEAnalysisStep* pstep = GetStep();
	char sztitle[256];

	if      (tag == "title"             ) { tag.value(sztitle); fem.SetTitle(sztitle); }
	else if (tag == "time_steps"        ) tag.value(pstep->m_ntime);
	else if (tag == "final_time"        ) tag.value(pstep->m_final_time);
	else if (tag == "step_size"         ) { tag.value(pstep->m_dt0); pstep->m_dt = pstep->m_dt0; }
	else if (tag == "optimize_bw"       ) tag.value(fem.m_bwopt);
	else if (tag == "pressure_stiffness") tag.value(pstep->m_istiffpr);
	else if (tag == "hourglass"         ) tag.value(FEUDGHexDomain::m_hg);
	else if (tag == "plane_strain"      )
	{
		int bc = 2;
		XMLAtt* patt = tag.Attribute("bc", true);
		if (patt)
		{
			XMLAtt& att = *patt;
			if      (att == "x") bc = 0;
			else if (att == "y") bc = 1;
			else if (att == "z") bc = 2;
			else throw XMLReader::InvalidAttributeValue(tag, "bc", att.cvalue());
		}
		bool b = false;
		tag.value(b);
		if (b) fem.m_nplane_strain = bc; else fem.m_nplane_strain = -1;
	}
	else if (tag == "analysis")
	{
		XMLAtt& att = tag.Attribute("type");
		if      (att == "static"      ) pstep->m_nanalysis = FE_STATIC;
		else if (att == "dynamic"     ) pstep->m_nanalysis = FE_DYNAMIC;
		else if (att == "steady-state") pstep->m_nanalysis = FE_STEADY_STATE;
		else throw XMLReader::InvalidAttributeValue(tag, "type", att.cvalue());
	}
	else if (tag == "restart" )
	{
		const char* szf = tag.AttributeValue("file", true);
		if (szf) m_pim->SetDumpfileName(szf);
		tag.value(pstep->m_bDump);
	}
	else if (tag == "time_stepper")
	{
		pstep->m_bautostep = true;
		++tag;
		do
		{
			if      (tag == "max_retries") tag.value(pstep->m_maxretries);
			else if (tag == "opt_iter"   ) tag.value(pstep->m_iteopt);
			else if (tag == "dtmin"      ) tag.value(pstep->m_dtmin);
			else if (tag == "dtmax"      )
			{
				tag.value(pstep->m_dtmax);
				const char* sz = tag.AttributeValue("lc", true);
				if (sz) pstep->m_nmplc = atoi(sz) - 1;
			}
			else if (tag == "aggressiveness") tag.value(pstep->m_naggr);
			else throw XMLReader::InvalidTag(tag);

			++tag;
		}
		while (!tag.isend());
	}
	else if (tag == "plot_level")
	{
		char szval[256];
		tag.value(szval);
		if		(strcmp(szval, "PLOT_DEFAULT"    ) == 0) {} // don't change the plot level
		else if (strcmp(szval, "PLOT_NEVER"      ) == 0) pstep->SetPlotLevel(FE_PLOT_NEVER);
		else if (strcmp(szval, "PLOT_MAJOR_ITRS" ) == 0) pstep->SetPlotLevel(FE_PLOT_MAJOR_ITRS);
		else if (strcmp(szval, "PLOT_MINOR_ITRS" ) == 0) pstep->SetPlotLevel(FE_PLOT_MINOR_ITRS);
		else if (strcmp(szval, "PLOT_MUST_POINTS") == 0) pstep->SetPlotLevel(FE_PLOT_MUST_POINTS);
		else if (strcmp(szval, "PLOT_FINAL"      ) == 0) pstep->SetPlotLevel(FE_PLOT_FINAL);
		else throw XMLReader::InvalidValue(tag);
	}
	else if (tag == "print_level")
	{
		char szval[256];
		tag.value(szval);
		if      (strcmp(szval, "PRINT_DEFAULT"       ) == 0) {} // don't change the default print level
		else if (strcmp(szval, "PRINT_NEVER"         ) == 0) pstep->SetPrintLevel(FE_PRINT_NEVER);
		else if (strcmp(szval, "PRINT_PROGRESS"      ) == 0) pstep->SetPrintLevel(FE_PRINT_PROGRESS);
		else if (strcmp(szval, "PRINT_MAJOR_ITRS"    ) == 0) pstep->SetPrintLevel(FE_PRINT_MAJOR_ITRS);
		else if (strcmp(szval, "PRINT_MINOR_ITRS"    ) == 0) pstep->SetPrintLevel(FE_PRINT_MINOR_ITRS);
		else if (strcmp(szval, "PRINT_MINOR_ITRS_EXP") == 0) pstep->SetPrintLevel(FE_PRINT_MINOR_ITRS_EXP);
		else throw XMLReader::InvalidTag(tag);
	}
	else if (tag == "use_three_field_hex") tag.value(m_pim->m_b3field);
	else if (tag == "integration")
	{
		++tag;
		do
		{
			if (tag == "rule")
			{
				XMLAtt& elem = tag.Attribute("elem");
				const char* szv = tag.szvalue();

				if (elem == "hex8")
				{
					if      (strcmp(szv, "GAUSS8") == 0) m_pim->m_nhex8 = FE_HEX8G8;
					else if (strcmp(szv, "POINT6") == 0) m_pim->m_nhex8 = FE_HEX8RI;
					else if (strcmp(szv, "UDG"   ) == 0) m_pim->m_nhex8 = FE_HEX8G1;
					else throw XMLReader::InvalidValue(tag);
				}
				else if (elem == "tet10")
				{
					if      (strcmp(szv, "GAUSS4"   ) == 0) m_pim->m_ntet10 = FE_TET10G4;
					else if (strcmp(szv, "GAUSS8"   ) == 0) m_pim->m_ntet10 = FE_TET10G8;
					else if (strcmp(szv, "LOBATTO11") == 0) m_pim->m_ntet10 = FE_TET10GL11;
					else throw XMLReader::InvalidValue(tag);
				}
				else if (elem == "tri3")
				{
					if      (strcmp(szv, "GAUSS1") == 0) m_pim->m_ntri3 = FE_TRI3G1;
					else if (strcmp(szv, "GAUSS3") == 0) m_pim->m_ntri3 = FE_TRI3G3;
					else throw XMLReader::InvalidValue(tag);
				}
				else if (elem == "tri6")
				{
					if      (strcmp(szv, "GAUSS3"  ) == 0) m_pim->m_ntri6 = FE_TRI6G3;
					else if (strcmp(szv, "GAUSS6"  ) == 0) m_pim->m_ntri6 = FE_TRI6NI;
					else if (strcmp(szv, "GAUSS4"  ) == 0) m_pim->m_ntri6 = FE_TRI6G4;
					else if (strcmp(szv, "GAUSS7"  ) == 0) m_pim->m_ntri6 = FE_TRI6G7;
					else if (strcmp(szv, "LOBATTO7") == 0) m_pim->m_ntri6 = FE_TRI6GL7;
					else throw XMLReader::InvalidValue(tag);
				}
				else if (elem == "tet4")
				{
					if (tag.isleaf())
					{
						if      (strcmp(szv, "GAUSS4") == 0) m_pim->m_ntet4 = FEFEBioImport::ET_TET4;
						else if (strcmp(szv, "GAUSS1") == 0) m_pim->m_ntet4 = FEFEBioImport::ET_TETG1;
						else if (strcmp(szv, "UT4"   ) == 0) m_pim->m_ntet4 = FEFEBioImport::ET_UT4;
						else throw XMLReader::InvalidValue(tag);
					}
					else
					{
						const char* szt = tag.AttributeValue("type");
						if      (strcmp(szt, "GAUSS4") == 0) m_pim->m_ntet4 = FEFEBioImport::ET_TET4;
						else if (strcmp(szt, "GAUSS1") == 0) m_pim->m_ntet4 = FEFEBioImport::ET_TETG1;
						else if (strcmp(szt, "UT4"   ) == 0) m_pim->m_ntet4 = FEFEBioImport::ET_UT4;
						else throw XMLReader::InvalidAttributeValue(tag, "type", szv);

						++tag;
						do
						{
							if      (tag == "alpha"   ) tag.value(FEUT4Domain::m_alpha);
							else if (tag == "iso_stab") tag.value(FEUT4Domain::m_bdev);
							else if (tag == "stab_int")
							{
								const char* sz = tag.szvalue();
								if (strcmp(sz, "GAUSS4") == 0) m_pim->m_nut4 = FE_TET4G4;
								else if (strcmp(sz, "GAUSS1") == 0) m_pim->m_nut4 = FE_TET4G1;
							}
							else throw XMLReader::InvalidTag(tag);
							++tag;
						}
						while (!tag.isend());
					}
				}
				else throw XMLReader::InvalidAttributeValue(tag, "elem", elem.cvalue());
			}
			else throw XMLReader::InvalidValue(tag);
			++tag;
		}
		while (!tag.isend());
	}
	else if (tag == "linear_solver")
	{
		XMLAtt& type = tag.Attribute("type");
		if      (type == "skyline") fem.m_nsolver = SKYLINE_SOLVER;
		else if (type == "psldlt" ) fem.m_nsolver = PSLDLT_SOLVER;
		else if (type == "superlu")
		{
			fem.m_nsolver = SUPERLU_SOLVER;
			if (!tag.isleaf())
			{
				SuperLUSolver* ps = new SuperLUSolver();
				FEAnalysisStep* pstep = GetStep();
				FESolver* psolver = dynamic_cast<FESolver*>(pstep->m_psolver);
				psolver->m_plinsolve = ps;

				++tag;
				do
				{
					if (tag == "print_cnorm") { bool b; tag.value(b); ps->print_cnorm(b); }
					else throw XMLReader::InvalidTag(tag);
					++tag;
				}
				while (!tag.isend());
			}
		}
		else if (type == "superlu_mt"        ) fem.m_nsolver = SUPERLU_MT_SOLVER;
		else if (type == "pardiso"           ) fem.m_nsolver = PARDISO_SOLVER;
		else if (type == "wsmp"              ) fem.m_nsolver = WSMP_SOLVER;
		else if (type == "lusolver"          ) fem.m_nsolver = LU_SOLVER;
		else if (type == "rcicg"             ) fem.m_nsolver = RCICG_SOLVER;
		else if (type == "conjugate gradient")
		{
			fem.m_nsolver = CG_ITERATIVE_SOLVER;
			ConjGradIterSolver* ps;
			FEAnalysisStep* pstep = GetStep();
			FESolver* psolver = dynamic_cast<FESolver*>(pstep->m_psolver);
			psolver->m_plinsolve = ps = new ConjGradIterSolver();
			if (!tag.isleaf())
			{
				++tag;
				do
				{
					if      (tag == "tolerance"     ) tag.value(ps->m_tol);
					else if (tag == "max_iterations") tag.value(ps->m_kmax);
					else if (tag == "print_level"   ) tag.value(ps->m_nprint);
					else throw XMLReader::InvalidTag(tag);
					++tag;
				}
				while (!tag.isend());
			}
		}
		else throw XMLReader::InvalidAttributeValue(tag, "type", type.cvalue());
	}
	else return false;

	return true;
}
