// =============================================================================
//  CADET - The Chromatography Analysis and Design Toolkit
//
//  Copyright © 2008-2020: The CADET Authors
//            Please see the AUTHORS and CONTRIBUTORS file.
//
//  All rights reserved. This program and the accompanying materials
//  are made available under the terms of the GNU Public License v3.0 (or, at
//  your option, any later version) which accompanies this distribution, and
//  is available at http://www.gnu.org/licenses/gpl.html
// =============================================================================

#include "model/ModelSystemImpl.hpp"
#include "cadet/ParameterProvider.hpp"

#include "AdUtils.hpp"
#include "SimulationTypes.hpp"

#include <iomanip>

#include "LoggingUtils.hpp"
#include "Logging.hpp"

#include "ParallelSupport.hpp"
#ifdef CADET_PARALLELIZE
	#include <tbb/tbb.h>
#endif

#include "model/ModelSystemImpl-Helper.hpp"

namespace
{
	struct FullTag {};
	struct LeanTag {};

	template <class tag_t>
	struct ConsistentInit {};

	template <>
	struct ConsistentInit<FullTag>
	{
		static inline void state(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, double* const vecStateY, const cadet::AdJacobianParams& adJac, double errorTol, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->consistentInitialState(simTime, vecStateY, adJac, errorTol, threadLocalMem);
		}

		static inline void timeDerivative(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, double const* vecStateY, double* const vecStateYdot, double* const res, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->consistentInitialTimeDerivative(simTime, vecStateY, vecStateYdot, threadLocalMem);
		}

		static inline int residualWithJacobian(cadet::model::ModelSystem& ms, const cadet::SimulationTime& simTime, const cadet::ConstSimulationState& simState, double* const res, double* const temp,
			const cadet::AdJacobianParams& adJac)
		{
			return ms.residualWithJacobian(simTime, simState, res, adJac);
		}

		static inline void parameterSensitivity(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, const cadet::ConstSimulationState& simState,
			std::vector<double*>& vecSensYlocal, std::vector<double*>& vecSensYdotLocal, cadet::active const* const adRes, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->consistentInitialSensitivity(simTime, simState, vecSensYlocal, vecSensYdotLocal, adRes, threadLocalMem);
		}
	};

	template <>
	struct ConsistentInit<LeanTag>
	{
		static inline void state(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, double* const vecStateY, const cadet::AdJacobianParams& adJac, double errorTol, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->leanConsistentInitialState(simTime, vecStateY, adJac, errorTol, threadLocalMem);
		}

		static inline void timeDerivative(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, double const* vecStateY, double* const vecStateYdot, double* const res, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->leanConsistentInitialTimeDerivative(simTime.t, vecStateY, vecStateYdot, res, threadLocalMem);
		}

		static inline int residualWithJacobian(cadet::model::ModelSystem& ms, const cadet::SimulationTime& simTime, const cadet::ConstSimulationState& simState, double* const res, double* const temp,
			const cadet::AdJacobianParams& adJac)
		{
			return ms.residualWithJacobian(simTime, simState, temp, adJac);
		}

		static inline void parameterSensitivity(cadet::IUnitOperation* model, const cadet::SimulationTime& simTime, const cadet::ConstSimulationState& simState,
			std::vector<double*>& vecSensYlocal, std::vector<double*>& vecSensYdotLocal, cadet::active const* const adRes, cadet::util::ThreadLocalStorage& threadLocalMem)
		{
			model->leanConsistentInitialSensitivity(simTime, simState, vecSensYlocal, vecSensYdotLocal, adRes, threadLocalMem);
		}
	};
}

namespace cadet
{

namespace model
{

int ModelSystem::dResDpFwdWithJacobian(const SimulationTime& simTime, const ConstSimulationState& simState,
	const AdJacobianParams& adJac)
{
	BENCH_SCOPE(_timerResidualSens);

	// Evaluate residual for all parameters using AD in vector mode and at the same time update the 
	// Jacobian (in one AD run, if analytic Jacobians are disabled)
#ifdef CADET_PARALLELIZE
	tbb::parallel_for(size_t(0), _models.size(), [&](size_t i)
#else
	for (unsigned int i = 0; i < _models.size(); ++i)
#endif
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];

		_errorIndicator[i] = m->residualSensFwdWithJacobian(simTime, applyOffset(simState, offset),
			applyOffset(adJac, offset), _threadLocalStorage);

	} CADET_PARFOR_END;

	// Handle connections
	residualConnectUnitOps<double, active, active>(simTime.secIdx, simState.vecStateY, simState.vecStateYdot, adJac.adRes);

	return totalErrorIndicatorFromLocal(_errorIndicator);
}

void ModelSystem::applyInitialCondition(const SimulationState& simState) const
{
	// If we have the full state vector available, use that and skip unit operations
	if (_initState.size() >= numDofs())
	{
		std::copy(_initState.data(), _initState.data() + numDofs(), simState.vecStateY);

		if (_initStateDot.size() >= numDofs())
			std::copy(_initStateDot.data(), _initStateDot.data() + numDofs(), simState.vecStateYdot);

		return;
	}

	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation const* const m = _models[i];
		const unsigned int offset = _dofOffset[i];
		m->applyInitialCondition(applyOffset(simState, offset));
	}
}

void ModelSystem::readInitialCondition(IParameterProvider& paramProvider)
{
	// Check if INIT_STATE_Y is present
	if (paramProvider.exists("INIT_STATE_Y"))
		_initState = paramProvider.getDoubleArray("INIT_STATE_Y");

	// Check if INIT_STATE_YDOT is present
	if (paramProvider.exists("INIT_STATE_YDOT"))
		_initStateDot = paramProvider.getDoubleArray("INIT_STATE_YDOT");

	std::ostringstream oss;
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];

		oss.str("");
		oss << "unit_" << std::setfill('0') << std::setw(3) << std::setprecision(0) << static_cast<int>(m->unitOperationId());

		const std::string subScope = oss.str();
		if (paramProvider.exists(subScope))
		{
			paramProvider.pushScope(subScope);
			m->readInitialCondition(paramProvider);
			paramProvider.popScope();
		}
	}
}

void ModelSystem::initializeSensitivityStates(const std::vector<double*>& vecSensY) const
{
	std::vector<double*> vecSensYlocal(vecSensY.size(), nullptr);
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];

		// Use correct offset in sensitivity state vectors
		for (unsigned int j = 0; j < vecSensY.size(); ++j)
			vecSensYlocal[j] = vecSensY[j] + offset;

		m->initializeSensitivityStates(vecSensYlocal);
	}
}

void ModelSystem::solveCouplingDOF(double* const vec)
{
	const unsigned int finalOffset = _dofOffset.back();

	// N_{f,x} Outlet (lower) matrices; Bottom macro-row
	// N_{f,x,1} * y_1 + ... + N_{f,x,nModels} * y_{nModels} + y_{coupling} = f
	// y_{coupling} = f - N_{f,x,1} * y_1 - ... - N_{f,x,nModels} * y_{nModels}
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		const unsigned int offset = _dofOffset[i];
		_jacFN[i].multiplySubtract(vec + offset, vec + finalOffset);
	}

	// Calculate inlet DOF for unit operations based on the coupling conditions. Depends on coupling conditions.
	// y_{unit op inlet} - y_{coupling} = 0
	// y_{unit op inlet} = y_{coupling}
	unsigned int idxCoupling = finalOffset;
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];
		if (!m->hasInlet())
			continue;
		
		for (unsigned int port = 0; port < m->numInletPorts(); ++port)
		{
			const unsigned int localIndex = m->localInletComponentIndex(port);
			const unsigned int localStride = m->localInletComponentStride(port);
			for (unsigned int comp = 0; comp < m->numComponents(); ++comp)
			{
				vec[offset + localIndex + comp*localStride] = vec[idxCoupling];
				++idxCoupling;
			}
		}
	}
}

template <typename tag_t>
void ModelSystem::consistentInitialConditionAlgorithm(const SimulationTime& simTime, const SimulationState& simState,
	const AdJacobianParams& adJac, double errorTol)
{
	BENCH_SCOPE(_timerConsistentInit);

	// Phase 1: Compute algebraic state variables

	// Consistent initial state for unit operations that only have outlets (system input, Inlet unit operation)
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];
		if (!m->hasInlet())
		{
			ConsistentInit<tag_t>::state(m, simTime, simState.vecStateY + offset, applyOffset(adJac, offset), errorTol, _threadLocalStorage);
		}
	}

	// Calculate coupling DOFs
	// These operations only requires correct unit operation outlet DOFs.
	// The outlets of the inlet unit operations have already been set above.
	// All other units are assumed to have correct outputs since their outlet DOFs are dynamic.
	const unsigned int finalOffset = _dofOffset.back();

	// Zero out the coupling DOFs (provides right hand side of 0 for solveCouplingDOF())
	std::fill(simState.vecStateY + finalOffset, simState.vecStateY + numDofs(), 0.0);

	solveCouplingDOF(simState.vecStateY);

	// Consistent initial state for all other unit operations (unit operations that have inlets)
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];
		if (m->hasInlet())
		{
			ConsistentInit<tag_t>::state(m, simTime, simState.vecStateY + offset, applyOffset(adJac, offset), errorTol, _threadLocalStorage);
		}
	}


	// Phase 2: Calculate residual with current state

	// Evaluate residual for right hand side without time derivatives \dot{y} and store it in vecStateYdot (or _tempState in case of lean initialization)
	// Also evaluate the Jacobian at the current position
	ConsistentInit<tag_t>::residualWithJacobian(*this, simTime, ConstSimulationState{simState.vecStateY, nullptr}, simState.vecStateYdot, _tempState, adJac);

	LOG(Debug) << "Residual post state: " << log::VectorPtr<double>(simState.vecStateYdot, numDofs());

	// Phase3 3: Calculate dynamic state variables yDot

	// Calculate all local yDot state variables
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];
		const unsigned int offset = _dofOffset[i];
		ConsistentInit<tag_t>::timeDerivative(m, simTime, simState.vecStateY + offset, simState.vecStateYdot + offset, _tempState + offset, _threadLocalStorage);
	}

	// Zero out the coupling DOFs (provides right hand side of 0 for solveCouplingDOF())
	std::fill(simState.vecStateYdot + finalOffset, simState.vecStateYdot + numDofs(), 0.0);
	// Calculate coupling DOFs
	solveCouplingDOF(simState.vecStateYdot);

	// Only enable this when you need to see the full jacobian for the system.
	// genJacobian(simTime, vecStateY, vecStateYdot);
}

void ModelSystem::consistentInitialConditions(const SimulationTime& simTime, const SimulationState& simState, 
	const AdJacobianParams& adJac, double errorTol)
{
	consistentInitialConditionAlgorithm<FullTag>(simTime, simState, adJac, errorTol);
}

void ModelSystem::consistentInitialSensitivity(const SimulationTime& simTime, 
	const ConstSimulationState& simState, std::vector<double*>& vecSensY, 
	std::vector<double*>& vecSensYdot, active* const adRes, active* const adY)
{
	consistentInitialSensitivityAlgorithm<FullTag>(simTime, simState, vecSensY, vecSensYdot, adRes, adY);
}

template <typename tag_t>
void ModelSystem::consistentInitialSensitivityAlgorithm(const SimulationTime& simTime, 
	const ConstSimulationState& simState, std::vector<double*>& vecSensY, 
	std::vector<double*>& vecSensYdot, active* const adRes, active* const adY)
{
	BENCH_SCOPE(_timerConsistentInit);

	// Compute parameter sensitivities and update the Jacobian
	dResDpFwdWithJacobian(simTime, simState, AdJacobianParams{adRes, adY, static_cast<unsigned int>(vecSensY.size())});

	std::vector<double*> vecSensYlocal(vecSensY.size(), nullptr);
	std::vector<double*> vecSensYdotLocal(vecSensYdot.size(), nullptr);
	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];

		if (!m->hasInlet())
		{
			const unsigned int offset = _dofOffset[i];

			// Use correct offset in sensitivity state vectors
			for (unsigned int j = 0; j < vecSensY.size(); ++j)
			{
				vecSensYlocal[j] = vecSensY[j] + offset;
				vecSensYdotLocal[j] = vecSensYdot[j] + offset;
			}

			ConsistentInit<tag_t>::parameterSensitivity(m, simTime, applyOffset(simState, offset), vecSensYlocal, vecSensYdotLocal, adRes + offset, _threadLocalStorage);
		}
	}

	const unsigned int finalOffset = _dofOffset.back();

	for (unsigned int param = 0; param < vecSensY.size(); ++param)
	{
		double* const vsy = vecSensY[param];
		for (unsigned int i = finalOffset; i < numDofs(); ++i)
			vsy[i] = -adRes[i].getADValue(param);

		solveCouplingDOF(vsy);
	}

	for (unsigned int i = 0; i < _models.size(); ++i)
	{
		IUnitOperation* const m = _models[i];

		if (m->hasInlet())
		{
			const unsigned int offset = _dofOffset[i];

			// Use correct offset in sensitivity state vectors
			for (unsigned int j = 0; j < vecSensY.size(); ++j)
			{
				vecSensYlocal[j] = vecSensY[j] + offset;
				vecSensYdotLocal[j] = vecSensYdot[j] + offset;
			}

			ConsistentInit<tag_t>::parameterSensitivity(m, simTime, applyOffset(simState, offset), vecSensYlocal, vecSensYdotLocal, adRes + offset, _threadLocalStorage);
		}
	}
		
	for (unsigned int i = 0; i < vecSensY.size(); ++i)
	{
		double* const vsyd = vecSensYdot[i];

		// Calculate -(d^2 res_con / (dy dp)) * \dot{y}
		if (_models.empty())
		{
			std::fill(vsyd + finalOffset, vsyd + numDofs(), 0.0);
		}
		else
		{
			ad::adMatrixVectorMultiply(_jacActiveFN[0], simState.vecStateYdot + _dofOffset[0], vsyd + finalOffset, -1.0, 0.0, i);
			for (unsigned int j = 1; j < _models.size(); ++j)
			{
				const unsigned int offset = _dofOffset[j];
				ad::adMatrixVectorMultiply(_jacActiveFN[j], simState.vecStateYdot + offset, vsyd + finalOffset, -1.0, 1.0, i);
			}
		}
		solveCouplingDOF(vsyd);
	}
}

void ModelSystem::leanConsistentInitialConditions(const SimulationTime& simTime, const SimulationState& simState, 
	const AdJacobianParams& adJac, double errorTol)
{
	consistentInitialConditionAlgorithm<LeanTag>(simTime, simState, adJac, errorTol);
}

void ModelSystem::leanConsistentInitialSensitivity(const SimulationTime& simTime, const ConstSimulationState& simState,
	std::vector<double*>& vecSensY, std::vector<double*>& vecSensYdot, active* const adRes, active* const adY)
{
	consistentInitialSensitivityAlgorithm<LeanTag>(simTime, simState, vecSensY, vecSensYdot, adRes, adY);
}

}  // namespace model

}  // namespace cadet
