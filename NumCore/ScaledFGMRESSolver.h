#pragma once
#include "FGMRES_ILU0_Solver.h"
#include "CompactUnSymmMatrix.h"

class ScaledFGMRESSolver : public FGMRES_ILU0_Solver
{
public:
	ScaledFGMRESSolver(FEModel* fem) : FGMRES_ILU0_Solver(fem)
	{
		m_k = 1.0;
	}

	bool Factor() override
	{
		CRSSparseMatrix* A = dynamic_cast<CRSSparseMatrix*>(GetSparseMatrix());
		if (A == nullptr) return false;

		int n0 = m_part[0];
		int n1 = m_part[1];

		// we want to multply all columns of partition 2 by 1/k
		for (CRSSparseMatrix::Iterator it(A); it.valid(); it.next())
		{
			MatrixItem item = it.get();
			if (item.col >= n0)
			{
				it.set(item.val / m_k);
			}
		}

		return FGMRES_ILU0_Solver::Factor();
	}

	bool BackSolve(double* x, double* b) override
	{
		bool ret = FGMRES_ILU0_Solver::BackSolve(x, b);
		if (ret == false) return false;

		int n0 = m_part[0];
		int n1 = m_part[1];
		for (size_t i = n0; i < n0+n1; ++i) x[i] /= m_k;

		return true;
	}

	void SetScaleFactor(double k) { m_k = k; }

protected:
	double			m_k;		//!< scale parameter
};