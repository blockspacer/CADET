// =============================================================================
//  CADET - The Chromatography Analysis and Design Toolkit
//  
//  Copyright © 2008-2019: The CADET Authors
//            Please see the AUTHORS and CONTRIBUTORS file.
//  
//  All rights reserved. This program and the accompanying materials
//  are made available under the terms of the GNU Public License v3.0 (or, at
//  your option, any later version) which accompanies this distribution, and
//  is available at http://www.gnu.org/licenses/gpl.html
// =============================================================================

#include <catch.hpp>
#include "Approx.hpp"
#include "cadet/ExternalFunction.hpp"
#include "cadet/ParameterProvider.hpp"

#define CADET_LOGGING_DISABLE
#include "Logging.hpp"

#include "JacobianHelper.hpp"
#include "ReactionModelTests.hpp"

#include "common/JsonParameterProvider.hpp"

#include "ReactionModelFactory.hpp"
#include "model/ReactionModel.hpp"
#include "linalg/DenseMatrix.hpp"
#include "linalg/BandMatrix.hpp"
#include "AdUtils.hpp"
#include "AutoDiff.hpp"

#include <algorithm>
#include <cstring>

namespace
{
	inline cadet::model::IDynamicReactionModel* createDynamicReactionModel(const char* name)
	{
		cadet::ReactionModelFactory rmf;
		cadet::model::IDynamicReactionModel* const rm = rmf.createDynamic(name);
		
		REQUIRE(nullptr != rm);
		return rm;
	}

	class ConstExternalFunction : public cadet::IExternalFunction
	{
	public:
		virtual bool configure(cadet::IParameterProvider* paramProvider) { return true; }
		virtual const char* name() const CADET_NOEXCEPT { return "CONSTFUN"; }
		virtual double externalProfile(double t, double z, double rho, double r, unsigned int sec) { return 1.0; }
		virtual double timeDerivative(double t, double z, double rho, double r, unsigned int sec) { return 0.0; }
		virtual void setSectionTimes(double const* secTimes, bool const* secContinuity, unsigned int nSections) { }
	};

	class LinearExternalFunction : public cadet::IExternalFunction
	{
	public:
		virtual bool configure(cadet::IParameterProvider* paramProvider) { return true; }
		virtual const char* name() const CADET_NOEXCEPT { return "LINFUN"; }
		virtual double externalProfile(double t, double z, double rho, double r, unsigned int sec) { return t; }
		virtual double timeDerivative(double t, double z, double rho, double r, unsigned int sec) { return 1.0; }
		virtual void setSectionTimes(double const* secTimes, bool const* secContinuity, unsigned int nSections) { }
	};

	class ConfiguredDynamicReactionModel
	{
	public:

		ConfiguredDynamicReactionModel(ConfiguredDynamicReactionModel&& cpy) CADET_NOEXCEPT 
			: _reaction(cpy._reaction), _nComp(cpy._nComp), _nBound(cpy._nBound), _boundOffset(cpy._boundOffset), _buffer(cpy._buffer), _extFuns(cpy._extFuns)
		{
			cpy._reaction = nullptr;
			cpy._nBound = nullptr;
			cpy._boundOffset = nullptr;
			cpy._buffer = nullptr;
			cpy._extFuns = nullptr;
		}

		~ConfiguredDynamicReactionModel()
		{
			if (_buffer)
				delete[] _buffer;
			if (_extFuns)
				delete[] _extFuns;
			delete[] _boundOffset;
			delete _reaction;
		}

		inline ConfiguredDynamicReactionModel& operator=(ConfiguredDynamicReactionModel&& cpy) CADET_NOEXCEPT
		{
			_reaction = cpy._reaction;
			_nComp = cpy._nComp;
			_nBound = cpy._nBound;
			_boundOffset = cpy._boundOffset;
			_buffer = cpy._buffer;
			_extFuns = cpy._extFuns;

			cpy._reaction = nullptr;
			cpy._nBound = nullptr;
			cpy._boundOffset = nullptr;
			cpy._buffer = nullptr;
			cpy._extFuns = nullptr;

			return *this;			
		}

		static ConfiguredDynamicReactionModel create(const char* name, unsigned int nComp, unsigned int const* nBound, const char* config)
		{
			cadet::model::IDynamicReactionModel* const rm = createDynamicReactionModel(name);

			// Calculate offset of bound states
			unsigned int* boundOffset = new unsigned int[nComp];
			boundOffset[0] = 0;
			for (unsigned int i = 1; i < nComp; ++i)
			{
				boundOffset[i] = boundOffset[i-1] + nBound[i-1];
			}
			const unsigned int totalBoundStates = boundOffset[nComp - 1] + nBound[nComp - 1];

			// Configure
			cadet::JsonParameterProvider jpp(config);
			rm->configureModelDiscretization(jpp, nComp, nBound, boundOffset);
			if (rm->requiresConfiguration())
			{
				jpp.set("EXTFUN", std::vector<int>(1, 0));
				REQUIRE(rm->configure(jpp, 0, 0));
			}

			// Assign external functions
			cadet::IExternalFunction* extFuns = new LinearExternalFunction[50];
			rm->setExternalFunctions(&extFuns, 50);

			// Allocate memory buffer
			unsigned int requiredMem = 0;
			if (rm->requiresWorkspace())
				requiredMem = rm->workspaceSize(nComp, totalBoundStates, boundOffset);

			char* buffer = nullptr;
			if (requiredMem > 0)
			{
				buffer = new char[requiredMem];
				std::memset(buffer, 0, requiredMem);
			}

			return ConfiguredDynamicReactionModel(rm, nComp, nBound, boundOffset, buffer, extFuns);
		}

		inline cadet::model::IDynamicReactionModel& model() { return *_reaction; }
		inline const cadet::model::IDynamicReactionModel& model() const { return *_reaction; }

		inline void* buffer() { return _buffer; }
		inline unsigned int nComp() const { return _nComp; }
		inline unsigned int const* nBound() const { return _nBound; }
		inline unsigned int const* boundOffset() const { return _boundOffset; }

		inline unsigned int numBoundStates() const { return _boundOffset[_nComp - 1] + _nBound[_nComp - 1]; }

	private:

		ConfiguredDynamicReactionModel(cadet::model::IDynamicReactionModel* reaction, unsigned int nComp, unsigned int const* nBound, unsigned int const* boundOffset, char* buffer, cadet::IExternalFunction* extFuns) 
			: _reaction(reaction), _nComp(nComp), _nBound(nBound), _boundOffset(boundOffset), _buffer(buffer), _extFuns(extFuns)
		{
		}

		cadet::model::IDynamicReactionModel* _reaction;
		unsigned int _nComp;
		unsigned int const* _nBound;
		unsigned int const* _boundOffset;
		char* _buffer;
		cadet::IExternalFunction* _extFuns;
	};
}

namespace cadet
{

namespace test
{

namespace reaction
{

void testDynamicJacobianAD(const char* modelName, unsigned int nComp, unsigned int const* nBound, const char* config, double const* point, double absTol, double relTol)
{
	ConfiguredDynamicReactionModel crm = ConfiguredDynamicReactionModel::create(modelName, nComp, nBound, config);

	const unsigned int numDofs = crm.nComp() + crm.numBoundStates();
	std::vector<double> yState(numDofs, 0.0);
	std::copy_n(point, numDofs, yState.data());

	std::vector<double> dir(numDofs, 0.0);
	std::vector<double> colA(numDofs, 0.0);
	std::vector<double> colB(numDofs, 0.0);

	// Enable AD
	cadet::ad::setDirections(cadet::ad::getMaxDirections());
	cadet::active* adRes = new cadet::active[numDofs];
	cadet::active* adY = new cadet::active[numDofs];

	// Combined liquid and solid phase

	// Evaluate with AD
	ad::prepareAdVectorSeedsForDenseMatrix(adY, 0, numDofs);
	ad::copyToAd(yState.data(), adY, numDofs);
	ad::resetAd(adRes, numDofs);
	crm.model().residualCombinedAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, adY, adRes, 1.0, crm.buffer());

	// Extract Jacobian
	cadet::linalg::DenseMatrix jacAD;
	jacAD.resize(numDofs, numDofs);
	ad::extractDenseJacobianFromAd(adRes, 0, jacAD);

	// Calculate analytic Jacobian
	cadet::linalg::DenseMatrix jacAna;
	jacAna.resize(numDofs, numDofs);
	crm.model().analyticJacobianCombinedAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, yState.data(), 1.0, jacAna.row(0), crm.buffer());

	cadet::test::checkJacobianPatternFD(
		[&](double const* lDir, double* res) -> void
			{
				std::fill_n(res, numDofs, 0.0);
				crm.model().residualCombinedAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, lDir, res, 1.0, crm.buffer());
			},
		[&](double const* lDir, double* res) -> void 
			{
				jacAna.multiplyVector(lDir, res);
			},
		yState.data(), dir.data(), colA.data(), colB.data(), numDofs, numDofs);

	cadet::test::checkJacobianPatternFD(
		[&](double const* lDir, double* res) -> void
			{
				std::fill_n(res, numDofs, 0.0);
				crm.model().residualCombinedAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, lDir, res, 1.0, crm.buffer());
			},
		[&](double const* lDir, double* res) -> void 
			{
				jacAD.multiplyVector(lDir, res);
			},
		yState.data(), dir.data(), colA.data(), colB.data(), numDofs, numDofs);

	// Check Jacobians against each other
	for (unsigned int row = 0; row < numDofs; ++row)
	{
		for (unsigned int col = 0; col < numDofs; ++col)
		{
			CAPTURE(row);
			CAPTURE(col);
			CHECK(jacAna.native(row, col) == makeApprox(jacAD.native(row, col), absTol, relTol));
		}
	}

	// Only liquid phase

	// Evaluate with AD
	ad::resetAd(adRes, numDofs);
	crm.model().residualLiquidAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, adY, adRes, 1.0, crm.buffer());

	// Extract Jacobian
	jacAD.setAll(0.0);
	ad::extractDenseJacobianFromAd(adRes, 0, jacAD);

	// Calculate analytic Jacobian
	jacAna.setAll(0.0);
	crm.model().analyticJacobianLiquidAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, yState.data(), 1.0, jacAna.row(0), crm.buffer());

	delete[] adY;
	delete[] adRes;

	cadet::test::checkJacobianPatternFD(
		[&](double const* lDir, double* res) -> void
			{
				std::fill_n(res, nComp, 0.0);
				crm.model().residualLiquidAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, lDir, res, 1.0, crm.buffer());
			},
		[&](double const* lDir, double* res) -> void 
			{
				jacAna.submatrixMultiplyVector(lDir, 0, 0, nComp, nComp, res);
			},
		yState.data(), dir.data(), colA.data(), colB.data(), nComp, nComp);

	cadet::test::checkJacobianPatternFD(
		[&](double const* lDir, double* res) -> void
			{
				std::fill_n(res, nComp, 0.0);
				crm.model().residualLiquidAdd(1.0, 0u, ColumnPosition{0.0, 0.0, 0.0}, lDir, res, 1.0, crm.buffer());
			},
		[&](double const* lDir, double* res) -> void 
			{
				jacAD.submatrixMultiplyVector(lDir, 0, 0, nComp, nComp, res);
			},
		yState.data(), dir.data(), colA.data(), colB.data(), nComp, nComp);

	// Check Jacobians against each other
	for (unsigned int row = 0; row < nComp; ++row)
	{
		for (unsigned int col = 0; col < nComp; ++col)
		{
			CAPTURE(row);
			CAPTURE(col);
			CHECK(jacAna.native(row, col) == makeApprox(jacAD.native(row, col), absTol, relTol));
		}
	}
}

} // namespace reaction
} // namespace test
} // namespace cadet
